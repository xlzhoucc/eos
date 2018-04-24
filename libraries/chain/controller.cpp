#include <eosio/chain/controller.hpp>
#include <eosio/chain/transaction_context.hpp>

#include <eosio/chain/block_log.hpp>
#include <eosio/chain/fork_database.hpp>

#include <eosio/chain/account_object.hpp>
#include <eosio/chain/scope_sequence_object.hpp>
#include <eosio/chain/block_summary_object.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/generated_transaction_object.hpp>
#include <eosio/chain/transaction_object.hpp>

#include <eosio/chain/authorization_manager.hpp>
#include <eosio/chain/resource_limits.hpp>

#include <chainbase/chainbase.hpp>
#include <fc/io/json.hpp>

#include <eosio/chain/eosio_contract.hpp>

namespace eosio { namespace chain {

using resource_limits::resource_limits_manager;


struct pending_state {
   pending_state( database::session&& s )
   :_db_session( move(s) ){}

   database::session                  _db_session;
   vector<transaction_metadata_ptr>   _applied_transaction_metas;

   block_state_ptr                    _pending_block_state;

   vector<action_receipt>             _actions;


   void push() {
      _db_session.push();
   }
};

struct controller_impl {
   controller&                    self;
   chainbase::database            db;
   block_log                      blog;
   optional<pending_state>        pending;
   block_state_ptr                head;
   fork_database                  fork_db;
   wasm_interface                 wasmif;
   resource_limits_manager        resource_limits;
   authorization_manager          authorization;
   controller::config             conf;

   typedef pair<scope_name,action_name>                   handler_key;
   map< account_name, map<handler_key, apply_handler> >   apply_handlers;

   /**
    *  Transactions that were undone by pop_block or abort_block, transactions
    *  are removed from this list if they are re-applied in other blocks. Producers
    *  can query this list when scheduling new transactions into blocks.
    */
   map<digest_type, transaction_metadata_ptr>     unapplied_transactions;

   block_id_type head_block_id()const {
      return head->id;
   }
   time_point head_block_time()const {
      return head->header.timestamp;
   }
   const block_header& head_block_header()const {
      return head->header;
   }

   void pop_block() {
      auto prev = fork_db.get_block( head->header.previous );
      FC_ASSERT( prev, "attempt to pop beyond last irreversible block" );
      for( const auto& t : head->trxs )
         unapplied_transactions[t->signed_id] = t;
      head = prev;
      db.undo();
   }


   void set_apply_handler( account_name contract, scope_name scope, action_name action, apply_handler v ) {
      apply_handlers[contract][make_pair(scope,action)] = v;
   }

   controller_impl( const controller::config& cfg, controller& s  )
   :self(s),
    db( cfg.shared_memory_dir,
        cfg.read_only ? database::read_only : database::read_write,
        cfg.shared_memory_size ),
    blog( cfg.block_log_dir ),
    fork_db( cfg.shared_memory_dir ),
    wasmif( cfg.wasm_runtime ),
    resource_limits( db ),
    authorization( s, db ),
    conf( cfg )
   {
      head = fork_db.head();


#define SET_APP_HANDLER( contract, scope, action, nspace ) \
   set_apply_handler( #contract, #scope, #action, &BOOST_PP_CAT(apply_, BOOST_PP_CAT(contract, BOOST_PP_CAT(_,action) ) ) )
   SET_APP_HANDLER( eosio, eosio, newaccount, eosio );
   SET_APP_HANDLER( eosio, eosio, setcode, eosio );
   SET_APP_HANDLER( eosio, eosio, setabi, eosio );
   SET_APP_HANDLER( eosio, eosio, updateauth, eosio );
   SET_APP_HANDLER( eosio, eosio, deleteauth, eosio );
   SET_APP_HANDLER( eosio, eosio, linkauth, eosio );
   SET_APP_HANDLER( eosio, eosio, unlinkauth, eosio );
   SET_APP_HANDLER( eosio, eosio, onerror, eosio );
   SET_APP_HANDLER( eosio, eosio, postrecovery, eosio );
   SET_APP_HANDLER( eosio, eosio, passrecovery, eosio );
   SET_APP_HANDLER( eosio, eosio, vetorecovery, eosio );
   SET_APP_HANDLER( eosio, eosio, canceldelay, eosio );


   }

   void init() {
      // ilog( "${c}", ("c",fc::json::to_pretty_string(cfg)) );
      add_indices();

      /**
      *  The fork database needs an initial block_state to be set before
      *  it can accept any new blocks. This initial block state can be found
      *  in the database (whose head block state should be irreversible) or
      *  it would be the genesis state.
      */
      if( !head ) {
         initialize_fork_db(); // set head to genesis state
#warning What if head is empty because the user deleted forkdb.dat? Will this not corrupt the database?
         db.set_revision( head->block_num );
         initialize_database();
      }

      FC_ASSERT( db.revision() == head->block_num, "fork database is inconsistent with shared memory",
                 ("db",db.revision())("head",head->block_num) );

      /**
       * The undoable state contains state transitions from blocks
       * in the fork database that could be reversed. Because this
       * is a new startup and the fork database is empty, we must
       * unwind that pending state. This state will be regenerated
       * when we catch up to the head block later.
       */
      //clear_all_undo();
   }

   ~controller_impl() {
      pending.reset();

      edump((db.revision())(head->block_num));

      db.flush();
   }

   void add_indices() {
      db.add_index<account_index>();
      db.add_index<account_sequence_index>();

      db.add_index<table_id_multi_index>();
      db.add_index<key_value_index>();
      db.add_index<index64_index>();
      db.add_index<index128_index>();
      db.add_index<index256_index>();
      db.add_index<index_double_index>();

      db.add_index<global_property_multi_index>();
      db.add_index<dynamic_global_property_multi_index>();
      db.add_index<block_summary_multi_index>();
      db.add_index<transaction_multi_index>();
      db.add_index<generated_transaction_multi_index>();
      db.add_index<scope_sequence_multi_index>();

      authorization.add_indices();
      resource_limits.add_indices();
   }

   void abort_pending_block() {
      pending.reset();
   }

   void clear_all_undo() {
      // Rewind the database to the last irreversible block
      db.with_write_lock([&] {
         db.undo_all();
         /*
         FC_ASSERT(db.revision() == self.head_block_num(),
                   "Chainbase revision does not match head block num",
                   ("rev", db.revision())("head_block", self.head_block_num()));
                   */
      });
   }

   /**
    *  Sets fork database head to the genesis state.
    */
   void initialize_fork_db() {
      wlog( " Initializing new blockchain with genesis state                  " );
      producer_schedule_type initial_schedule{ 0, {{N(eosio), conf.genesis.initial_key}} };

      block_header_state genheader;
      genheader.active_schedule       = initial_schedule;
      genheader.pending_schedule      = initial_schedule;
      genheader.pending_schedule_hash = fc::sha256::hash(initial_schedule);
      genheader.header.timestamp      = conf.genesis.initial_timestamp;
      genheader.header.action_mroot   = conf.genesis.compute_chain_id();
      genheader.id                    = genheader.header.id();
      genheader.block_num             = genheader.header.block_num();

      head = std::make_shared<block_state>( genheader );
      signed_block genblock(genheader.header);

      edump((genheader.header));
      edump((genblock));
      blog.append( genblock );

      fork_db.set( head );
   }

   void create_native_account( account_name name, const authority& owner, const authority& active, bool is_privileged = false ) {
      db.create<account_object>([&](auto& a) {
         a.name = name;
         a.creation_date = conf.genesis.initial_timestamp;
         a.privileged = is_privileged;

         if( name == config::system_account_name ) {
            a.set_abi(eosio_contract_abi(abi_def()));
         }
      });
      db.create<account_sequence_object>([&](auto & a) {
        a.name = name;
      });

      const auto& owner_permission  = authorization.create_permission(name, config::owner_name, 0,
                                                                      owner, conf.genesis.initial_timestamp );
      const auto& active_permission = authorization.create_permission(name, config::active_name, owner_permission.id,
                                                                      active, conf.genesis.initial_timestamp );

      resource_limits.initialize_account(name);
      resource_limits.add_pending_account_ram_usage(
         name,
         (int64_t)(config::billable_size_v<permission_object> + owner_permission.auth.get_billable_size())
      );
      resource_limits.add_pending_account_ram_usage(
         name,
         (int64_t)(config::billable_size_v<permission_object> + active_permission.auth.get_billable_size())
      );
   }

   void initialize_database() {
      // Initialize block summary index
      for (int i = 0; i < 0x10000; i++)
         db.create<block_summary_object>([&](block_summary_object&) {});

      const auto& tapos_block_summary = db.get<block_summary_object>(1);
      db.modify( tapos_block_summary, [&]( auto& bs ) {
        bs.block_id = head->id;
      });

      db.create<global_property_object>([&](auto& gpo ){
        gpo.configuration = conf.genesis.initial_configuration;
      });
      db.create<dynamic_global_property_object>([](auto&){});

      authorization.initialize_database();
      resource_limits.initialize_database();

      authority system_auth(conf.genesis.initial_key);
      create_native_account( config::system_account_name, system_auth, system_auth, true );

      auto empty_authority = authority(0, {}, {});
      auto active_producers_authority = authority(0, {}, {});
      active_producers_authority.accounts.push_back({{config::system_account_name, config::active_name}, 1});

      create_native_account( config::nobody_account_name, empty_authority, empty_authority );
      create_native_account( config::producers_account_name, empty_authority, active_producers_authority );
   }

   void set_pending_tapos() {
      const auto& tapos_block_summary = db.get<block_summary_object>((uint16_t)pending->_pending_block_state->block_num);
      db.modify( tapos_block_summary, [&]( auto& bs ) {
        bs.block_id = pending->_pending_block_state->id;
      });
   }

   void commit_block( bool add_to_fork_db ) {
      set_pending_tapos();
      resource_limits.process_account_limit_updates();
      resource_limits.process_block_usage( pending->_pending_block_state->block_num );

      if( add_to_fork_db ) {
         pending->_pending_block_state->validated = true;
         auto new_bsp = fork_db.add( pending->_pending_block_state );
         head = fork_db.head();
         FC_ASSERT( new_bsp == head, "committed block did not become the new head in fork database" );
      }

      pending->push();
      pending.reset();
      self.accepted_block( head );
   }

   transaction_trace_ptr apply_onerror( const generated_transaction_object& gto, fc::time_point deadline, uint32_t cpu_usage ) {
      signed_transaction etrx;
      etrx.actions.emplace_back(vector<permission_level>{{gto.sender,config::active_name}},
                                onerror( gto.sender_id, gto.packed_trx.data(), gto.packed_trx.size()) );


      etrx.expiration = self.pending_block_time() + fc::seconds(1);
      etrx.set_reference_block( self.head_block_id() );

      transaction_context trx_context( self, etrx, etrx.id() );
      trx_context.deadline = deadline;
      /// TODO: forward cpu usage into error trx_context.cpu_usage = cpu_usage;
      trx_context.is_input  = false;
      trx_context.exec();

      db.remove( gto );
      trx_context.squash();

      return move(trx_context.trace);
   }

   void push_scheduled_transaction( const generated_transaction_object& gto, fc::time_point deadline  ) {
      fc::datastream<const char*> ds( gto.packed_trx.data(), gto.packed_trx.size() );

      FC_ASSERT( gto.delay_until <= self.pending_block_time() );

      optional<fc::exception> soft_except;
      optional<fc::exception> hard_except;

      transaction_trace_ptr trace;
      uint32_t apply_cpu_usage = 0;
      try {
         signed_transaction dtrx;
         fc::raw::unpack(ds,static_cast<transaction&>(dtrx) );

         transaction_context trx_context( self, dtrx, gto.trx_id );
         trace = trx_context.trace;

         trx_context.deadline  = deadline;
         trx_context.published = gto.published;
         trx_context.net_usage = 0;
         trx_context.apply_context_free = false;
         trx_context.is_input           = false;
         try {
            trx_context.exec();
         } catch ( ... ) {
            apply_cpu_usage = trx_context.trace->cpu_usage;
            throw;
         }

         fc::move_append( pending->_actions, move(trx_context.executed) );

         trx_context.trace->receipt = push_receipt( gto.trx_id, transaction_receipt::executed, trx_context.trace->kcpu_usage(), 0 );

         db.remove( gto );
         trx_context.squash();
         self.applied_transaction( trx_context.trace );
      } catch( const fc::exception& e ) {
         soft_except = e;
      }
      if( soft_except && gto.sender != account_name() ) { /// TODO: soft errors should not go to error handlers (deadline error)
         edump((soft_except->to_detail_string()));
         try {
            auto trace = apply_onerror( gto, deadline, apply_cpu_usage  );
            trace->soft_except = soft_except;
            self.applied_transaction( trace );
         } catch ( const fc::exception& e ) {
            hard_except = e;
         }
      }

      edump((hard_except->to_detail_string()));
      db.remove( gto );
      trace->receipt  = push_receipt( gto.trx_id, transaction_receipt::hard_fail, (apply_cpu_usage+999)/1000, 0 );
      trace->soft_except = soft_except;
      trace->hard_except = hard_except;
      self.applied_transaction( trace );
   } /// push_scheduled_transaction


   /**
    *  Adds the transaction receipt to the pending block and returns it.
    */
   template<typename T>
   const transaction_receipt& push_receipt( const T& trx, transaction_receipt_header::status_enum status,
                      uint32_t kcpu_usage, uint32_t net_usage_words ) {
      pending->_pending_block_state->block->transactions.emplace_back( trx );
      transaction_receipt& r = pending->_pending_block_state->block->transactions.back();
      r.kcpu_usage           = kcpu_usage;
      r.net_usage_words      = net_usage_words;
      r.status               = status;
      return r;
   }

   void push_unapplied_transaction( fc::time_point deadline ) {
      auto itr = unapplied_transactions.begin();
      if( itr == unapplied_transactions.end() )  
         return;

      push_transaction( itr->second, deadline );
   }


   /**
    *  This is the entry point for new transactions to the block state. It will check authorization and
    *  determine whether to execute it now or to delay it. Lastly it inserts a transaction receipt into
    *  the pending block.
    */
   void push_transaction( const transaction_metadata_ptr& trx, 
                          fc::time_point deadline = fc::time_point::maximum(), 
                          bool implicit = false ) {
      if( deadline == fc::time_point() ) {
         unapplied_transactions[trx->id] = trx;
         return;
      }

      transaction_trace_ptr trace;
      try {
         unapplied_transactions.erase( trx->signed_id );

         transaction_context trx_context( self, trx->trx, trx->id );
         trace = trx_context.trace;

         auto required_delay = authorization.check_authorization( trx->trx.actions, trx->recover_keys() );

         trx_context.deadline  = deadline;
         trx_context.published = self.pending_block_time();
         trx_context.net_usage = self.validate_net_usage( trx );
         trx_context.is_input  = !implicit;
         trx_context.delay = std::max( fc::seconds(trx->trx.delay_sec), required_delay );
         trx_context.exec();

         fc::move_append( pending->_actions, move(trx_context.executed) );


         const auto& trace = trx_context.trace;
         if( !implicit ) {
            if( trx_context.delay == fc::seconds(0) ) {
               trace->receipt = push_receipt( trx->packed_trx, transaction_receipt::executed, trace->kcpu_usage(), trx_context.net_usage );
            } else {
               trace->receipt = push_receipt( trx->packed_trx, transaction_receipt::delayed, trace->kcpu_usage(), trx_context.net_usage );
            }
         }

         pending->_pending_block_state->trxs.emplace_back(trx);
         self.accepted_transaction(trx);
         trx_context.squash();
      } catch ( const fc::exception& e ) {
         trace->soft_except = e;
      }

      if( trx->on_result ) {
         (*trx->on_result)(trace);
      }
   } /// push_transaction


   void start_block( block_timestamp_type when ) {
     FC_ASSERT( !pending );

     FC_ASSERT( db.revision() == head->block_num, "",
                ("db_head_block", db.revision())("controller_head_block", head->block_num)("fork_db_head_block", fork_db.head()->block_num) );

     pending = db.start_undo_session(true);
     pending->_pending_block_state = std::make_shared<block_state>( *head, when );
     pending->_pending_block_state->in_current_chain = true;

     try {
        auto onbtrx = std::make_shared<transaction_metadata>( get_on_block_transaction() );
        push_transaction( onbtrx, fc::time_point::maximum(), true );
     } catch ( ... ) {
        ilog( "on block transaction failed, but shouldn't impact block generation, system contract needs update" );
     }
   } // start_block



   void sign_block( const std::function<signature_type( const digest_type& )>& signer_callback ) {
      auto p = pending->_pending_block_state;
      p->sign( signer_callback );
      static_cast<signed_block_header&>(*p->block) = p->header;
   } /// sign_block

   void apply_block( const signed_block_ptr& b ) {
      try {
         start_block( b->timestamp );

         for( const auto& receipt : b->transactions ) {
            if( receipt.trx.contains<packed_transaction>() ) {
               auto& pt = receipt.trx.get<packed_transaction>();
               auto mtrx = std::make_shared<transaction_metadata>(pt);
               push_transaction( mtrx );
            }
         }

         finalize_block();
         sign_block( [&]( const auto& ){ return b->producer_signature; } );

         // this is implied by the signature passing
         //FC_ASSERT( b->id() == pending->_pending_block_state->block->id(),
         //           "applying block didn't produce expected block id" );

         commit_block(false);
         return;
      } catch ( const fc::exception& e ) {
         edump((e.to_detail_string()));
         abort_block();
         throw;
      }
   } /// apply_block


   void push_block( const signed_block_ptr& b ) {
      auto new_header_state = fork_db.add( b );
      self.accepted_block_header( new_header_state );
      maybe_switch_forks();
   }

   void push_confirmation( const header_confirmation& c ) {
      fork_db.add( c );
      self.accepted_confirmation( c );
      maybe_switch_forks();
   }

   void maybe_switch_forks() {
      auto new_head = fork_db.head();

      if( new_head->header.previous == head->id ) {
         try {
            abort_block();
            apply_block( new_head->block );
            fork_db.mark_in_current_chain( new_head, true );
            fork_db.set_validity( new_head, true );
            head = new_head;
         } catch ( const fc::exception& e ) {
            fork_db.set_validity( new_head, false ); // Removes new_head from fork_db index, so no need to mark it as not in the current chain.
            throw;
         }
      } else if( new_head->id != head->id ) {
         wlog("switching forks from ${current_head_id} (block number ${current_head_num}) to ${new_head_id} (block number ${new_head_num})",
              ("current_head_id", head->id)("current_head_num", head->block_num)("new_head_id", new_head->id)("new_head_num", new_head->block_num) );
         auto branches = fork_db.fetch_branch_from( new_head->id, head->id );

         for( auto itr = branches.second.begin(); itr != branches.second.end(); ++itr ) {
            fork_db.mark_in_current_chain( *itr , false );
            pop_block();
         }
         FC_ASSERT( head_block_id() == branches.second.back()->header.previous,
                    "loss of sync between fork_db and chainbase during fork switch" ); // _should_ never fail

         for( auto ritr = branches.first.rbegin(); ritr != branches.first.rend(); ++ritr) {
            optional<fc::exception> except;
            try {
               apply_block( (*ritr)->block );
               head = *ritr;
               fork_db.mark_in_current_chain( *ritr, true );
            }
            catch (const fc::exception& e) { except = e; }
            if (except) {
               wlog("exception thrown while switching forks ${e}", ("e",except->to_detail_string()));

               while (ritr != branches.first.rend() ) {
                  fork_db.set_validity( *ritr, false );
                  ++ritr;
               }

               // pop all blocks from the bad fork
               for( auto itr = (ritr + 1).base(); itr != branches.second.end(); ++itr ) {
                  fork_db.mark_in_current_chain( *itr , false );
                  pop_block();
               }
               FC_ASSERT( head_block_id() == branches.second.back()->header.previous,
                          "loss of sync between fork_db and chainbase during fork switch reversal" ); // _should_ never fail

               // re-apply good blocks
               for( auto ritr = branches.second.rbegin(); ritr != branches.second.rend(); ++ritr ) {
                  apply_block( (*ritr)->block );
                  head = *ritr;
                  fork_db.mark_in_current_chain( *ritr, true );
               }
               throw *except;
            } // end if exception
         } /// end for each block in branch
         wlog("successfully switched fork to new head ${new_head_id}", ("new_head_id", new_head->id));
      }
   } /// push_block

   void abort_block() {
      if( pending ) {
         for( const auto& t : pending->_applied_transaction_metas )
            unapplied_transactions[t->signed_id] = t;
         pending.reset();
      }
   }


   bool should_enforce_runtime_limits()const {
      return false;
   }

   void set_action_merkle() {
      vector<digest_type> action_digests;
      action_digests.reserve( pending->_actions.size() );
      for( const auto& a : pending->_actions )
         action_digests.emplace_back( a.digest() );

      pending->_pending_block_state->header.action_mroot = merkle( move(action_digests) );
   }

   void set_trx_merkle() {
      vector<digest_type> trx_digests;
      const auto& trxs = pending->_pending_block_state->block->transactions;
      trx_digests.reserve( trxs.size() );
      for( const auto& a : trxs )
         trx_digests.emplace_back( a.digest() );

      pending->_pending_block_state->header.transaction_mroot = merkle( move(trx_digests) );
   }


   void finalize_block()
   { try {
      ilog( "finalize block ${n} (${id}) at ${t} by ${p} (${signing_key}); schedule_version: ${v} lib: ${lib} ${np}",
            ("n",pending->_pending_block_state->block_num)
            ("id",pending->_pending_block_state->header.id())
            ("t",pending->_pending_block_state->header.timestamp)
            ("p",pending->_pending_block_state->header.producer)
            ("signing_key", pending->_pending_block_state->block_signing_key)
            ("v",pending->_pending_block_state->header.schedule_version)
            ("lib",pending->_pending_block_state->dpos_last_irreversible_blocknum)
            ("np",pending->_pending_block_state->header.new_producers)
            );

      set_action_merkle();
      set_trx_merkle();

      auto p = pending->_pending_block_state;
      p->id = p->header.id();

      create_block_summary();


      /* TODO RESTORE
      const auto& b = trace.block;
      update_global_properties( b );
      update_global_dynamic_data( b );
      update_signing_producer(signing_producer, b);

      create_block_summary(b);
      clear_expired_transactions();

      update_last_irreversible_block();

      resource_limits.process_account_limit_updates();

      const auto& chain_config = self.get_global_properties().configuration;
      resource_limits.set_block_parameters(
         {EOS_PERCENT(chain_config.max_block_cpu_usage, chain_config.target_block_cpu_usage_pct), chain_config.max_block_cpu_usage, config::block_cpu_usage_average_window_ms / config::block_interval_ms, 1000, {99, 100}, {1000, 999}},
         {EOS_PERCENT(chain_config.max_block_net_usage, chain_config.target_block_net_usage_pct), chain_config.max_block_net_usage, config::block_size_average_window_ms / config::block_interval_ms, 1000, {99, 100}, {1000, 999}}
      );

      */
   } FC_CAPTURE_AND_RETHROW() }


   void create_block_summary() {
      auto p = pending->_pending_block_state;
      auto sid = p->block_num & 0xffff;
      db.modify( db.get<block_summary_object,by_id>(sid), [&](block_summary_object& bso ) {
          bso.block_id = p->id;
      });
   }

   void clear_expired_transactions() {
      //Look for expired transactions in the deduplication list, and remove them.
      auto& transaction_idx = db.get_mutable_index<transaction_multi_index>();
      const auto& dedupe_index = transaction_idx.indices().get<by_expiration>();
      while( (!dedupe_index.empty()) && (head_block_time() > fc::time_point(dedupe_index.begin()->expiration) ) ) {
         transaction_idx.remove(*dedupe_index.begin());
      }

      // Look for expired transactions in the pending generated list, and remove them.
      // TODO: expire these by sending error to handler
      auto& generated_transaction_idx = db.get_mutable_index<generated_transaction_multi_index>();
      const auto& generated_index = generated_transaction_idx.indices().get<by_expiration>();
      while( (!generated_index.empty()) && (head_block_time() > generated_index.begin()->expiration) ) {
      // TODO:   destroy_generated_transaction(*generated_index.begin());
      }
   }

   /*
   bool should_check_tapos()const { return true; }

   void validate_tapos( const transaction& trx )const {
      if( !should_check_tapos() ) return;

      const auto& tapos_block_summary = db.get<block_summary_object>((uint16_t)trx.ref_block_num);

      //Verify TaPoS block summary has correct ID prefix, and that this block's time is not past the expiration
      EOS_ASSERT(trx.verify_reference_block(tapos_block_summary.block_id), invalid_ref_block_exception,
                 "Transaction's reference block did not match. Is this transaction from a different fork?",
                 ("tapos_summary", tapos_block_summary));
   }
   */


   /**
    *  At the start of each block we notify the system contract with a transaction that passes in
    *  the block header of the prior block (which is currently our head block)
    */
   signed_transaction get_on_block_transaction()
   {
      action on_block_act;
      on_block_act.account = config::system_account_name;
      on_block_act.name = N(onblock);
      on_block_act.authorization = vector<permission_level>{{config::system_account_name, config::active_name}};
      on_block_act.data = fc::raw::pack(head_block_header());

      signed_transaction trx;
      trx.actions.emplace_back(std::move(on_block_act));
      trx.set_reference_block(head_block_id());
      trx.expiration = head_block_time() + fc::seconds(1);
      return trx;
   }

}; /// controller_impl

const resource_limits_manager&   controller::get_resource_limits_manager()const
{
   return my->resource_limits;
}
resource_limits_manager&         controller::get_mutable_resource_limits_manager()
{
   return my->resource_limits;
}

const authorization_manager&   controller::get_authorization_manager()const
{
   return my->authorization;
}
authorization_manager&         controller::get_mutable_authorization_manager()
{
   return my->authorization;
}

controller::controller( const controller::config& cfg )
:my( new controller_impl( cfg, *this ) )
{
   my->init();
}

controller::~controller() {
}


void controller::startup() {
   my->head = my->fork_db.head();
   if( !my->head ) {
   }

   /*
   auto head = my->blog.read_head();
   if( head && head_block_num() < head->block_num() ) {
      wlog( "\nDatabase in inconsistant state, replaying block log..." );
      //replay();
   }
   */
}

chainbase::database& controller::db()const { return my->db; }


void controller::start_block( block_timestamp_type when ) {
   my->start_block(when);
}

void controller::finalize_block() {
   my->finalize_block();
}

void controller::sign_block( const std::function<signature_type( const digest_type& )>& signer_callback ) {
   my->sign_block( signer_callback );
}

void controller::commit_block() {
   my->commit_block(true);
}

block_state_ptr controller::head_block_state()const {
   return my->head;
}

block_state_ptr controller::pending_block_state()const {
   if( my->pending ) return my->pending->_pending_block_state;
   return block_state_ptr();
}

void controller::abort_block() {
   my->abort_block();
}

void controller::push_block( const signed_block_ptr& b ) {
   my->push_block( b );
}

void controller::push_confirmation( const header_confirmation& c ) {
   my->push_confirmation( c );
}

void controller::push_transaction( const transaction_metadata_ptr& trx, fc::time_point deadline ) {
   my->push_transaction(trx, deadline);
}
void controller::push_unapplied_transaction( fc::time_point deadline ) {
   my->push_unapplied_transaction( deadline );
}

transaction_trace_ptr controller::sync_push( const transaction_metadata_ptr& trx ) {
   transaction_trace_ptr trace;
   trx->on_result = [&]( const transaction_trace_ptr& t ){ trace = t; }; 
   my->push_transaction( trx );
   return trace;
}

void controller::push_next_scheduled_transaction( fc::time_point deadline ) {
   const auto& idx = db().get_index<generated_transaction_multi_index,by_delay>();
   if( idx.begin() != idx.end() ) 
      my->push_scheduled_transaction( *idx.begin(), deadline );
}
void controller::push_scheduled_transaction( const transaction_id_type& trxid, fc::time_point deadline ) {
   /// lookup scheduled trx and then apply it...
}

uint32_t controller::head_block_num()const {
   return my->head->block_num;
}
block_id_type controller::head_block_id()const {
   return my->head->id;
}

time_point controller::head_block_time()const {
   return my->head_block_time();
}

time_point controller::pending_block_time()const {
   FC_ASSERT( my->pending, "no pending block" );
   return my->pending->_pending_block_state->header.timestamp;
}

const dynamic_global_property_object& controller::get_dynamic_global_properties()const {
  return my->db.get<dynamic_global_property_object>();
}
const global_property_object& controller::get_global_properties()const {
  return my->db.get<global_property_object>();
}

/**
 *  This method reads the current dpos_irreverible block number, if it is higher
 *  than the last block number of the log, it grabs the next block from the
 *  fork database, saves it to disk, then removes the block from the fork database.
 *
 *  Any forks built off of a different block with the same number are also pruned.
 */
void controller::log_irreversible_blocks() {
   if( !my->blog.head() )
      my->blog.read_head();

   const auto& log_head = my->blog.head();
   auto lib = my->head->dpos_last_irreversible_blocknum;

   if( lib > 1 ) {
      while( log_head && log_head->block_num() < lib ) {
         auto lhead = log_head->block_num();
         auto blk = my->fork_db.get_block_in_current_chain_by_num( lhead + 1 );
         FC_ASSERT( blk, "unable to find block state", ("block_num",lhead+1));
         irreversible_block( blk );
         my->blog.append( *blk->block );
         my->fork_db.prune( blk );
         my->db.commit( lhead );
      }
   }
}
signed_block_ptr controller::fetch_block_by_id( block_id_type id )const {
   auto state = my->fork_db.get_block(id);
   if( state ) return state->block;
   auto bptr = fetch_block_by_number( block_header::num_from_id(id) );
   if( bptr->id() == id ) return bptr;
   return signed_block_ptr();
}

signed_block_ptr controller::fetch_block_by_number( uint32_t block_num )const  {
   optional<signed_block> b = my->blog.read_block_by_num(block_num);
   if( b ) return std::make_shared<signed_block>( move(*b) );

   auto blk_state = my->fork_db.get_block_in_current_chain_by_num( block_num );
   if( blk_state ) return blk_state->block;
   return signed_block_ptr();
}

void controller::pop_block() {
   my->pop_block();
}

void controller::set_active_producers( const producer_schedule_type& sch ) {
   FC_ASSERT( !my->pending->_pending_block_state->header.new_producers, "this block has already set new producers" );
   FC_ASSERT( !my->pending->_pending_block_state->pending_schedule.producers.size(), "there is already a pending schedule, wait for it to become active" );
   my->pending->_pending_block_state->set_new_producers( sch );
}
const producer_schedule_type& controller::active_producers()const {
   return my->pending->_pending_block_state->active_schedule;
}

const producer_schedule_type& controller::pending_producers()const {
   return my->pending->_pending_block_state->pending_schedule;
}

const apply_handler* controller::find_apply_handler( account_name receiver, account_name scope, action_name act ) const
{
   auto native_handler_scope = my->apply_handlers.find( receiver );
   if( native_handler_scope != my->apply_handlers.end() ) {
      auto handler = native_handler_scope->second.find( make_pair( scope, act ) );
      if( handler != native_handler_scope->second.end() )
         return &handler->second;
   }
   return nullptr;
}
wasm_interface& controller::get_wasm_interface() {
   return my->wasmif;
}

const account_object& controller::get_account( account_name name )const
{ try {
   return my->db.get<account_object, by_name>(name);
} FC_CAPTURE_AND_RETHROW( (name) ) }

const map<digest_type, transaction_metadata_ptr>&  controller::unapplied_transactions()const {
   return my->unapplied_transactions;
}


void controller::validate_referenced_accounts( const transaction& trx )const {
   for( const auto& a : trx.context_free_actions ) {
      get_account( a.account );
      FC_ASSERT( a.authorization.size() == 0 );
   }
   bool one_auth = false;
   for( const auto& a : trx.actions ) {
      get_account( a.account );
      for( const auto& auth : a.authorization ) {
         one_auth = true;
         get_account( auth.actor );
      }
   }
   EOS_ASSERT( one_auth, tx_no_auths, "transaction must have at least one authorization" );
}

void controller::validate_expiration( const transaction& trx )const { try {
   const auto& chain_configuration = get_global_properties().configuration;

   EOS_ASSERT( time_point(trx.expiration) >= pending_block_time(), expired_tx_exception, "transaction has expired" );
   EOS_ASSERT( time_point(trx.expiration) <= pending_block_time() + fc::seconds(chain_configuration.max_transaction_lifetime),
               tx_exp_too_far_exception,
               "Transaction expiration is too far in the future relative to the reference time of ${reference_time}, "
               "expiration is ${trx.expiration} and the maximum transaction lifetime is ${max_til_exp} seconds",
               ("trx.expiration",trx.expiration)("reference_time",pending_block_time())
               ("max_til_exp",chain_configuration.max_transaction_lifetime) );
} FC_CAPTURE_AND_RETHROW((trx)) }

uint64_t controller::validate_net_usage( const transaction_metadata_ptr& trx )const {
   const auto& cfg = get_global_properties().configuration;

   auto actual_net_usage = cfg.base_per_transaction_net_usage + trx->packed_trx.get_billable_size();

   actual_net_usage = ((actual_net_usage + 7)/8) * 8; // Round up to nearest multiple of 8

   uint32_t net_usage_limit = trx->trx.max_net_usage_words.value * 8UL; // overflow checked in validate_transaction_without_state
   EOS_ASSERT( net_usage_limit == 0 || actual_net_usage <= net_usage_limit, tx_resource_exhausted,
               "declared net usage limit of transaction is too low: ${actual_net_usage} > ${declared_limit}",
               ("actual_net_usage", actual_net_usage)("declared_limit",net_usage_limit) );

   return actual_net_usage;
}

void controller::validate_tapos( const transaction& trx )const { try {
   const auto& tapos_block_summary = db().get<block_summary_object>((uint16_t)trx.ref_block_num);

   //Verify TaPoS block summary has correct ID prefix, and that this block's time is not past the expiration
   EOS_ASSERT(trx.verify_reference_block(tapos_block_summary.block_id), invalid_ref_block_exception,
              "Transaction's reference block did not match. Is this transaction from a different fork?",
              ("tapos_summary", tapos_block_summary));
} FC_CAPTURE_AND_RETHROW() }


} } /// eosio::chain