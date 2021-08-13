#include "BTreeVI.hpp"

#include "leanstore/concurrency-recovery/CRMG.hpp"
// -------------------------------------------------------------------------------------
#include "gflags/gflags.h"
// -------------------------------------------------------------------------------------
#include <signal.h>
// -------------------------------------------------------------------------------------
using namespace std;
using namespace leanstore::storage;
using OP_RESULT = leanstore::OP_RESULT;
// -------------------------------------------------------------------------------------
// Assumptions made in this implementation:
// 1) We don't insert an already removed key
// 2) Secondary Versions contain delta
// Keep in mind that garbage collection may leave pages completely empty
namespace leanstore
{
namespace storage
{
namespace btree
{
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::lookup(u8* o_key, u16 o_key_length, function<void(const u8*, u16)> payload_callback)
{
   const u16 key_length = o_key_length + sizeof(ChainSN);
   u8 key[key_length];
   std::memcpy(key, o_key, o_key_length);
   *reinterpret_cast<ChainSN*>(key + o_key_length) = 0;
   // -------------------------------------------------------------------------------------
   if (cr::activeTX().isSerializable()) {
      return lookupPessimistic(key, key_length, payload_callback);
   }
   const OP_RESULT ret = lookupOptimistic(key, key_length, payload_callback);
   if (ret == OP_RESULT::OTHER) {
      return lookupPessimistic(key, key_length, payload_callback);
   } else {
      return ret;
   }
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::lookupPessimistic(u8* key_buffer, const u16 key_length, function<void(const u8*, u16)> payload_callback)
{
   MutableSlice m_key(key_buffer, key_length);
   Slice key(key_buffer, key_length);
   jumpmuTry()
   {
      BTreeSharedIterator iterator(*static_cast<BTreeGeneric*>(this),
                                   cr::activeTX().isSerializable() ? LATCH_FALLBACK_MODE::EXCLUSIVE : LATCH_FALLBACK_MODE::SHARED);
      auto ret = iterator.seekExact(key);
      explainIfNot(ret == OP_RESULT::OK);
      if (ret != OP_RESULT::OK) {
         jumpmu_return OP_RESULT::NOT_FOUND;
      }
      [[maybe_unused]] const auto primary_version = *reinterpret_cast<const ChainedTuple*>(iterator.value().data());
      auto reconstruct = reconstructTuple(iterator, m_key, [&](Slice value) { payload_callback(value.data(), value.length()); });
      COUNTERS_BLOCK()
      {
         WorkerCounters::myCounters().cc_read_chains[dt_id]++;
         WorkerCounters::myCounters().cc_read_versions_visited[dt_id] += std::get<1>(reconstruct);
      }
      ret = std::get<0>(reconstruct);
      if (ret != OP_RESULT::ABORT_TX && ret != OP_RESULT::OK) {  // For debugging
         cout << endl;
         cout << u64(std::get<1>(reconstruct)) << endl;
         raise(SIGTRAP);
      }
      jumpmu_return ret;
   }
   jumpmuCatch() {}
   UNREACHABLE();
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::lookupOptimistic(const u8* key, const u16 key_length, function<void(const u8*, u16)> payload_callback)
{
   while (true) {
      jumpmuTry()
      {
         HybridPageGuard<BTreeNode> leaf;
         findLeafCanJump(leaf, key, key_length);
         // -------------------------------------------------------------------------------------
         s16 pos = leaf->lowerBound<true>(key, key_length);
         if (pos != -1) {
            auto& tuple = *reinterpret_cast<Tuple*>(leaf->getPayload(pos));
            if (isVisibleForMe(tuple.worker_id, tuple.worker_commit_mark, false)) {
               u32 offset = 0;
               if (tuple.tuple_format == TupleFormat::CHAINED) {
                  offset = sizeof(ChainedTuple);
               } else if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
                  offset = sizeof(FatTupleDifferentAttributes);
               } else {
                  leaf.recheck();
                  UNREACHABLE();
               }
               payload_callback(leaf->getPayload(pos) + offset, leaf->getPayloadLength(pos) - offset);
               leaf.recheck();
               jumpmu_return OP_RESULT::OK;
            } else {
               jumpmu_break;
            }
         } else {
            leaf.recheck();
            raise(SIGTRAP);
            jumpmu_return OP_RESULT::NOT_FOUND;
         }
      }
      jumpmuCatch() {}
   }
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::updateSameSizeInPlace(u8* o_key,
                                         u16 o_key_length,
                                         function<void(u8* value, u16 value_size)> callback,
                                         UpdateSameSizeInPlaceDescriptor& update_descriptor)
{
   assert(!cr::activeTX().isReadOnly());
   cr::Worker::my().walEnsureEnoughSpace(PAGE_SIZE * 1);
   const u16 key_length = o_key_length + sizeof(ChainSN);
   u8 key_buffer[key_length];
   std::memcpy(key_buffer, o_key, o_key_length);
   Slice key(key_buffer, key_length);
   MutableSlice m_key(key_buffer, key_length);
   setSN(m_key, 0);
   OP_RESULT ret;
   // -------------------------------------------------------------------------------------
   // 20K instructions more
   jumpmuTry()
   {
      BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(this));
      ret = iterator.seekExact(key);
      if (ret != OP_RESULT::OK) {
         raise(SIGTRAP);
         jumpmu_return ret;
      }
      // -------------------------------------------------------------------------------------
   restart : {
      MutableSlice primary_payload = iterator.mutableValue();
      auto& tuple = *reinterpret_cast<Tuple*>(primary_payload.data());
      if (tuple.isWriteLocked() || !isVisibleForMe(tuple.worker_id, tuple.worker_commit_mark)) {
         jumpmu_return OP_RESULT::ABORT_TX;
      }
      if (cr::activeTX().isSerializable()) {
         if (FLAGS_2pl) {
            if (tuple.read_lock_counter > 0 && tuple.read_lock_counter != (1ull << cr::Worker::my().workerID())) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
         } else {
            if (tuple.read_ts > cr::activeTX().TTS()) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
         }
      }
      tuple.writeLock();
      COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_chains[dt_id]++; }
      // -------------------------------------------------------------------------------------
      if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
         const bool res =
             reinterpret_cast<FatTupleDifferentAttributes*>(&tuple)->update(iterator, o_key, o_key_length, callback, update_descriptor, *this);
         ensure(res);  // TODO: what if it fails, then we have to do something else
         // Attention: tuple pointer is not valid here
         reinterpret_cast<Tuple*>(iterator.mutableValue().data())->unlock();
         // -------------------------------------------------------------------------------------
         if (cr::activeTX().isSingleStatement()) {
            cr::Worker::my().commitTX();
         }
         // -------------------------------------------------------------------------------------
         iterator.contentionSplit();
         // -------------------------------------------------------------------------------------
         jumpmu_return OP_RESULT::OK;
      }
      auto& chain_head = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
      if (!FLAGS_mv || FLAGS_vi_fupdate_chained) {
         // Single version
         // WAL
         u16 delta_and_descriptor_size = update_descriptor.size() + update_descriptor.diffLength();
         auto wal_entry = iterator.leaf.reserveWALEntry<WALUpdateSSIP>(o_key_length + delta_and_descriptor_size);
         wal_entry->type = WAL_LOG_TYPE::WALUpdate;
         wal_entry->key_length = o_key_length;
         wal_entry->delta_length = delta_and_descriptor_size;
         wal_entry->before_worker_id = chain_head.worker_id;
         wal_entry->before_worker_commit_mark = chain_head.worker_commit_mark;
         wal_entry->after_worker_id = cr::Worker::my().workerID();
         wal_entry->after_worker_commit_mark = cr::activeTX().TTS();
         std::memcpy(wal_entry->payload, o_key, o_key_length);
         std::memcpy(wal_entry->payload + o_key_length, &update_descriptor, update_descriptor.size());
         BTreeLL::generateDiff(update_descriptor, wal_entry->payload + o_key_length + update_descriptor.size(), chain_head.payload);
         callback(chain_head.payload, primary_payload.length() - sizeof(ChainedTuple));
         BTreeLL::generateXORDiff(update_descriptor, wal_entry->payload + o_key_length + update_descriptor.size(), chain_head.payload);
         wal_entry.submit();
         // -------------------------------------------------------------------------------------
         chain_head.worker_id = cr::Worker::my().workerID();
         chain_head.worker_commit_mark = cr::activeTX().TTS();
         tuple.unlock();
         // -------------------------------------------------------------------------------------
         iterator.contentionSplit();
         jumpmu_return OP_RESULT::OK;
      }
      // -------------------------------------------------------------------------------------
      bool convert_to_fat_tuple = FLAGS_vi_fat_tuple && chain_head.can_convert_to_fat_tuple &&
                                  !(chain_head.worker_id == cr::Worker::my().workerID() && chain_head.worker_commit_mark == cr::activeTX().TTS());
      if (convert_to_fat_tuple) {
         const u64 random_number = utils::RandomGenerator::getRandU64();
         convert_to_fat_tuple &= ((random_number & ((1ull << FLAGS_vi_fat_tuple_threshold) - 1)) == 0);
      }
      if (convert_to_fat_tuple) {
         ensure(chain_head.isWriteLocked());
         const bool convert_ret = convertChainedToFatTupleDifferentAttributes(iterator, m_key);
         if (convert_ret) {
            COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_fat_tuple_convert[dt_id]++; }
         }
         goto restart;
         UNREACHABLE();
      }
   }
      // -------------------------------------------------------------------------------------
      // Update in chained mode
      u16 delta_and_descriptor_size = update_descriptor.size() + update_descriptor.diffLength();
      u16 secondary_payload_length = delta_and_descriptor_size + sizeof(ChainedTupleVersion);
      u8 secondary_payload[PAGE_SIZE];
      auto& secondary_version = *reinterpret_cast<ChainedTupleVersion*>(secondary_payload);
      std::memcpy(secondary_version.payload, &update_descriptor, update_descriptor.size());
      // -------------------------------------------------------------------------------------
      ChainSN secondary_sn;
      // -------------------------------------------------------------------------------------
      DanglingPointer dangling_pointer;
      {
         MutableSlice head_payload = iterator.mutableValue();
         ChainedTuple& head_version = *reinterpret_cast<ChainedTuple*>(head_payload.data());
         // -------------------------------------------------------------------------------------
         dangling_pointer.bf = iterator.leaf.bf;
         dangling_pointer.latch_version_should_be = iterator.leaf.guard.latch->version + 1;
         dangling_pointer.head_slot = iterator.cur;
         dangling_pointer.remove_operation = false;
         // -------------------------------------------------------------------------------------
         BTreeLL::generateDiff(update_descriptor, secondary_version.payload + update_descriptor.size(), head_version.payload);
         // -------------------------------------------------------------------------------------
         new (secondary_payload) ChainedTupleVersion(head_version.worker_id, head_version.worker_commit_mark, false, true, cr::activeTX().TTS());
         secondary_version.next_sn = head_version.next_sn;
         if (secondary_version.worker_id == cr::Worker::my().workerID() && secondary_version.worker_commit_mark == cr::activeTX().TTS()) {
            secondary_version.committed_before_sat = std::numeric_limits<u64>::max();
         } else {
            secondary_version.committed_before_sat = cr::activeTX().TTS();
         }
         // -------------------------------------------------------------------------------------
         if (head_version.next_sn <= 1) {
            secondary_sn = leanstore::utils::RandomGenerator::getRand<ChainSN>(1, std::numeric_limits<ChainSN>::max());
         } else {
            secondary_sn = leanstore::utils::RandomGenerator::getRand<ChainSN>(1, head_version.next_sn);
         }
         iterator.markAsDirty();
      }
      // -------------------------------------------------------------------------------------
      // Write the ChainedTupleDelta
      while (true) {
         setSN(m_key, secondary_sn);
         ret = iterator.insertKV(key, Slice(secondary_payload, secondary_payload_length));
         if (ret == OP_RESULT::OK) {
            iterator.leaf->gc_space_used += iterator.leaf->getKVConsumedSpace(iterator.cur);
            break;
         } else {
            secondary_sn = leanstore::utils::RandomGenerator::getRand<ChainSN>(1, std::numeric_limits<ChainSN>::max());
         }
      }
      COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_versions_created[dt_id]++; }
      iterator.markAsDirty();
      // -------------------------------------------------------------------------------------
      {
         // Return to the head
         MutableSlice head_payload(nullptr, 0);
         if (dangling_pointer.bf == iterator.leaf.bf && dangling_pointer.latch_version_should_be == (iterator.leaf.guard.latch->version + 1)) {
            dangling_pointer.secondary_slot = iterator.cur;
            dangling_pointer.valid = true;
            iterator.cur = dangling_pointer.head_slot;
            head_payload = iterator.mutableValue();
         } else {
            setSN(m_key, 0);
            ret = iterator.seekExactWithHint(key, false);
            ensure(ret == OP_RESULT::OK);
            head_payload = iterator.mutableValue();
         }
         ChainedTuple& head_version = *reinterpret_cast<ChainedTuple*>(head_payload.data());
         // -------------------------------------------------------------------------------------
         // Head WTTS if needed
         u64 head_wtts = 0;
         // TODO: head_wtts = cr::Worker::composeWIDCM(head_version.worker_id, head_version.worker_commit_mark);
         // WAL
         auto wal_entry = iterator.leaf.reserveWALEntry<WALUpdateSSIP>(o_key_length + delta_and_descriptor_size);
         wal_entry->type = WAL_LOG_TYPE::WALUpdate;
         wal_entry->key_length = o_key_length;
         wal_entry->delta_length = delta_and_descriptor_size;
         wal_entry->before_worker_id = head_version.worker_id;
         wal_entry->before_worker_commit_mark = head_version.worker_commit_mark;
         wal_entry->after_worker_id = cr::Worker::my().workerID();
         wal_entry->after_worker_commit_mark = cr::activeTX().TTS();
         std::memcpy(wal_entry->payload, o_key, o_key_length);
         std::memcpy(wal_entry->payload + o_key_length, &update_descriptor, update_descriptor.size());
         BTreeLL::generateDiff(update_descriptor, wal_entry->payload + o_key_length + update_descriptor.size(), head_version.payload);
         callback(head_version.payload, head_payload.length() - sizeof(ChainedTuple));  // Update
         BTreeLL::generateXORDiff(update_descriptor, wal_entry->payload + o_key_length + update_descriptor.size(), head_version.payload);
         wal_entry.submit();
         // -------------------------------------------------------------------------------------
         head_version.worker_id = cr::Worker::my().workerID();
         head_version.worker_commit_mark = cr::activeTX().TTS();
         head_version.next_sn = secondary_sn;
         // -------------------------------------------------------------------------------------
         if (FLAGS_vi_utodo) {
            cr::Worker::my().stageTODO(
                head_version.worker_id, head_version.worker_commit_mark, dt_id, key_length + sizeof(TODOPoint),
                [&](u8* entry) {
                   auto& todo_entry = *new (entry) TODOPoint();
                   todo_entry.key_length = o_key_length;
                   todo_entry.sn = secondary_sn;
                   todo_entry.dangling_pointer = dangling_pointer;
                   std::memcpy(todo_entry.key, o_key, o_key_length);
                },
                head_wtts);
         }
         // -------------------------------------------------------------------------------------
         if (cr::activeTX().isSerializable()) {
            if (FLAGS_2pl) {
               // Nothing, the WorkerID + Commit HWM are the write lock
               head_version.read_lock_counter = (1ull << cr::Worker::my().workerID());
            } else {
               head_version.read_ts = cr::activeTX().TTS();
            }
         }
         // -------------------------------------------------------------------------------------
         head_version.unlock();
         iterator.contentionSplit();
         // -------------------------------------------------------------------------------------
         if (cr::activeTX().isSingleStatement()) {
            cr::Worker::my().commitTX();
         }
         // -------------------------------------------------------------------------------------
         jumpmu_return OP_RESULT::OK;
      }
   }
   jumpmuCatch() {}
   UNREACHABLE();
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::insert(u8* o_key, u16 o_key_length, u8* value, u16 value_length)
{
   assert(!cr::activeTX().isReadOnly());
   cr::Worker::my().walEnsureEnoughSpace(PAGE_SIZE * 1);
   const u16 key_length = o_key_length + sizeof(ChainSN);
   u8 key_buffer[key_length];
   std::memcpy(key_buffer, o_key, o_key_length);
   MutableSlice m_key(key_buffer, key_length);
   Slice key(key_buffer, key_length);
   setSN(m_key, 0);
   const u16 payload_length = value_length + sizeof(ChainedTuple);
   // -------------------------------------------------------------------------------------
   while (true) {
      jumpmuTry()
      {
         BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(this));
         OP_RESULT ret = iterator.seekToInsert(key);
         if (ret == OP_RESULT::DUPLICATE) {
            MutableSlice primary_payload = iterator.mutableValue();
            auto& primary_version = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
            if (primary_version.isWriteLocked() || !isVisibleForMe(primary_version.worker_id, primary_version.worker_commit_mark)) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
            ensure(false);  // Not implemented: maybe it has been removed but no GCed
         }
         ret = iterator.enoughSpaceInCurrentNode(key, payload_length);
         if (ret == OP_RESULT::NOT_ENOUGH_SPACE) {
            iterator.splitForKey(key);
            jumpmu_continue;
         }
         // -------------------------------------------------------------------------------------
         // WAL
         auto wal_entry = iterator.leaf.reserveWALEntry<WALInsert>(o_key_length + value_length);
         wal_entry->type = WAL_LOG_TYPE::WALInsert;
         wal_entry->key_length = o_key_length;
         wal_entry->value_length = value_length;
         std::memcpy(wal_entry->payload, o_key, o_key_length);
         std::memcpy(wal_entry->payload + o_key_length, value, value_length);
         wal_entry.submit();
         // -------------------------------------------------------------------------------------
         iterator.insertInCurrentNode(key, payload_length);
         MutableSlice payload = iterator.mutableValue();
         auto& primary_version = *new (payload.data()) ChainedTuple(cr::Worker::my().workerID(), cr::activeTX().TTS());
         std::memcpy(primary_version.payload, value, value_length);
         // -------------------------------------------------------------------------------------
         if (cr::activeTX().isSingleStatement()) {
            cr::Worker::my().commitTX();
         }
         // -------------------------------------------------------------------------------------
         jumpmu_return OP_RESULT::OK;
      }
      jumpmuCatch() { UNREACHABLE(); }
   }
   UNREACHABLE();
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::remove(u8* o_key, u16 o_key_length)
{
   // TODO: remove fat tuple
   assert(!cr::activeTX().isReadOnly());
   cr::Worker::my().walEnsureEnoughSpace(PAGE_SIZE * 1);
   u8 key_buffer[o_key_length + sizeof(ChainSN)];
   const u16 key_length = o_key_length + sizeof(ChainSN);
   std::memcpy(key_buffer, o_key, o_key_length);
   MutableSlice m_key(key_buffer, key_length);
   Slice key(key_buffer, key_length);
   setSN(m_key, 0);
   // -------------------------------------------------------------------------------------
   jumpmuTry()
   {
      BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(this));
      OP_RESULT ret = iterator.seekExact(key);
      if (ret != OP_RESULT::OK) {
         raise(SIGTRAP);
         jumpmu_return OP_RESULT::NOT_FOUND;
      }
      // -------------------------------------------------------------------------------------
      if (FLAGS_vi_fremove) {
         ret = iterator.removeCurrent();
         ensure(ret == OP_RESULT::OK);
         iterator.mergeIfNeeded();
         jumpmu_return OP_RESULT::OK;
      }
      // -------------------------------------------------------------------------------------
      u16 value_length, secondary_payload_length;
      u8 secondary_payload[PAGE_SIZE];
      auto& secondary_version = *reinterpret_cast<ChainedTupleVersion*>(secondary_payload);
      ChainSN secondary_sn;
      // -------------------------------------------------------------------------------------
      DanglingPointer dangling_pointer;
      {
         auto primary_payload = iterator.mutableValue();
         ChainedTuple& primary_version = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
         // -------------------------------------------------------------------------------------
         dangling_pointer.bf = iterator.leaf.bf;
         dangling_pointer.latch_version_should_be = iterator.leaf.guard.version + 1;
         dangling_pointer.head_slot = iterator.cur;
         dangling_pointer.remove_operation = true;
         // -------------------------------------------------------------------------------------
         ensure(primary_version.tuple_format == TupleFormat::CHAINED);  // TODO: removing fat tuple is not supported atm
         if (primary_version.isWriteLocked() || !isVisibleForMe(primary_version.worker_id, primary_version.worker_commit_mark)) {
            jumpmu_return OP_RESULT::ABORT_TX;
         }
         if (cr::activeTX().isSerializable()) {
            if (FLAGS_2pl) {
               if (primary_version.read_lock_counter > 0 && primary_version.read_lock_counter != (1ull << cr::Worker::my().workerID())) {
                  jumpmu_return OP_RESULT::ABORT_TX;
               }
            } else {
               if (primary_version.read_ts > cr::activeTX().TTS()) {
                  jumpmu_return OP_RESULT::ABORT_TX;
               }
            }
         }
         ensure(!cr::activeTX().atLeastSI() || primary_version.is_removed == false);
         if (primary_version.is_removed) {
            jumpmu_return OP_RESULT::NOT_FOUND;
         }
         primary_version.writeLock();
         // -------------------------------------------------------------------------------------
         value_length = iterator.value().length() - sizeof(ChainedTuple);
         secondary_payload_length = sizeof(ChainedTupleVersion) + value_length;
         new (secondary_payload)
             ChainedTupleVersion(primary_version.worker_id, primary_version.worker_commit_mark, false, false, cr::activeTX().TTS());
         secondary_version.worker_id = primary_version.worker_id;
         secondary_version.worker_commit_mark = primary_version.worker_commit_mark;
         secondary_version.next_sn = primary_version.next_sn;
         std::memcpy(secondary_version.payload, primary_payload.data(), value_length);
         iterator.markAsDirty();
      }
      // -------------------------------------------------------------------------------------
      {
         do {
            secondary_sn = leanstore::utils::RandomGenerator::getRand<ChainSN>(0, std::numeric_limits<ChainSN>::max());
            // -------------------------------------------------------------------------------------
            setSN(m_key, secondary_sn);
            ret = iterator.insertKV(key, Slice(secondary_payload, secondary_payload_length));
         } while (ret != OP_RESULT::OK);
      }
      iterator.leaf->gc_space_used += iterator.leaf->getKVConsumedSpace(iterator.cur);
      iterator.markAsDirty();
      // -------------------------------------------------------------------------------------
      {
         // Return to the head
         MutableSlice primary_payload(nullptr, 0);
         if (dangling_pointer.bf == iterator.leaf.bf && dangling_pointer.latch_version_should_be == (iterator.leaf.guard.version + 1)) {
            dangling_pointer.secondary_slot = iterator.cur;
            dangling_pointer.valid = true;
            iterator.cur = dangling_pointer.head_slot;
            ensure(dangling_pointer.secondary_slot > dangling_pointer.head_slot);
            primary_payload = iterator.mutableValue();
         } else {
            setSN(m_key, 0);
            ret = iterator.seekExactWithHint(key, false);
            ensure(ret == OP_RESULT::OK);
            primary_payload = iterator.mutableValue();
         }
         ChainedTuple old_primary_version = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
         // -------------------------------------------------------------------------------------
         // WAL
         auto wal_entry = iterator.leaf.reserveWALEntry<WALRemove>(o_key_length + value_length);
         wal_entry->type = WAL_LOG_TYPE::WALRemove;
         wal_entry->key_length = o_key_length;
         wal_entry->value_length = value_length;
         wal_entry->before_worker_id = old_primary_version.worker_id;
         wal_entry->before_worker_commit_mark = old_primary_version.worker_commit_mark;
         std::memcpy(wal_entry->payload, o_key, o_key_length);
         std::memcpy(wal_entry->payload + o_key_length, iterator.value().data(), value_length);
         wal_entry.submit();
         // -------------------------------------------------------------------------------------
         if (primary_payload.length() - sizeof(ChainedTuple) > 1) {
            iterator.shorten(sizeof(ChainedTuple));
         }
         primary_payload = iterator.mutableValue();
         auto& primary_version = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
         primary_version = old_primary_version;
         primary_version.is_removed = true;
         primary_version.worker_id = cr::Worker::my().workerID();
         primary_version.worker_commit_mark = cr::activeTX().TTS();
         primary_version.next_sn = secondary_sn;
         if (cr::activeTX().isSerializable()) {
            if (FLAGS_2pl) {
               primary_version.read_lock_counter = (1ull << cr::Worker::my().workerID());
            } else {
               primary_version.read_ts = cr::activeTX().TTS();
            }
         }
         // -------------------------------------------------------------------------------------
         if (FLAGS_vi_rtodo) {
            const u64 wtts = cr::Worker::composeWIDCM(old_primary_version.worker_id, old_primary_version.worker_commit_mark);
            cr::Worker::my().stageTODO(
                cr::Worker::my().workerID(), cr::activeTX().TTS(), dt_id, key_length + sizeof(TODOPoint),
                [&](u8* entry) {
                   auto& todo_entry = *new (entry) TODOPoint();
                   todo_entry.key_length = o_key_length;
                   todo_entry.dangling_pointer = dangling_pointer;
                   std::memcpy(todo_entry.key, o_key, o_key_length);
                },
                wtts);
         }
         iterator.leaf->gc_space_used += iterator.leaf->getKVConsumedSpace(iterator.cur);
         primary_version.unlock();
      }
      // -------------------------------------------------------------------------------------
      if (cr::activeTX().isSingleStatement()) {
         cr::Worker::my().commitTX();
      }
      // -------------------------------------------------------------------------------------
      jumpmu_return OP_RESULT::OK;
   }
   jumpmuCatch() {}
   UNREACHABLE();
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
// This undo implementation works only for rollback and not for undo operations during recovery
void BTreeVI::undo(void* btree_object, const u8* wal_entry_ptr, const u64)
{
   auto& btree = *reinterpret_cast<BTreeVI*>(btree_object);
   static_cast<void>(btree);
   const WALEntry& entry = *reinterpret_cast<const WALEntry*>(wal_entry_ptr);
   switch (entry.type) {
      case WAL_LOG_TYPE::WALInsert: {  // Assuming no insert after remove
         auto& insert_entry = *reinterpret_cast<const WALInsert*>(&entry);
         jumpmuTry()
         {
            const u16 key_length = insert_entry.key_length + sizeof(ChainSN);
            u8 key_buffer[key_length];
            std::memcpy(key_buffer, insert_entry.payload, insert_entry.key_length);
            *reinterpret_cast<ChainSN*>(key_buffer + insert_entry.key_length) = 0;
            Slice key(key_buffer, key_length);
            // -------------------------------------------------------------------------------------
            BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
            auto ret = iterator.seekExact(key);
            ensure(ret == OP_RESULT::OK);
            ret = iterator.removeCurrent();
            ensure(ret == OP_RESULT::OK);
            iterator.markAsDirty();  // TODO: write CLS
            iterator.mergeIfNeeded();
         }
         jumpmuCatch() {}
         break;
      }
      case WAL_LOG_TYPE::WALUpdate: {
         auto& update_entry = *reinterpret_cast<const WALUpdateSSIP*>(&entry);
         jumpmuTry()
         {
            const u16 key_length = update_entry.key_length + sizeof(ChainSN);
            u8 key_buffer[key_length];
            std::memcpy(key_buffer, update_entry.payload, update_entry.key_length);
            Slice key(key_buffer, key_length);
            MutableSlice m_key(key_buffer, key_length);
            // -------------------------------------------------------------------------------------
            ChainSN undo_sn;
            OP_RESULT ret;
            u8 secondary_payload[PAGE_SIZE];
            u16 secondary_payload_length;
            // -------------------------------------------------------------------------------------
            BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
            btree.setSN(m_key, 0);
            ret = iterator.seekExact(key);
            ensure(ret == OP_RESULT::OK);
            {
               auto& tuple = *reinterpret_cast<Tuple*>(iterator.mutableValue().data());
               ensure(!tuple.isWriteLocked());
               if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
                  reinterpret_cast<FatTupleDifferentAttributes*>(iterator.mutableValue().data())->undoLastUpdate();
                  jumpmu_return;
               }
            }
            // -------------------------------------------------------------------------------------
            if (!FLAGS_mv) {
               auto& chain_head = *reinterpret_cast<ChainedTuple*>(iterator.mutableValue().data());
               ensure(!chain_head.isWriteLocked());
               ensure(chain_head.tuple_format == TupleFormat::CHAINED);
               // -------------------------------------------------------------------------------------
               chain_head.worker_id = update_entry.before_worker_id;
               chain_head.worker_commit_mark = update_entry.before_worker_commit_mark;
               const auto& update_descriptor =
                   *reinterpret_cast<const UpdateSameSizeInPlaceDescriptor*>(update_entry.payload + update_entry.key_length);
               BTreeLL::applyXORDiff(update_descriptor, chain_head.payload,
                                     update_entry.payload + update_entry.key_length + update_descriptor.size());
               // -------------------------------------------------------------------------------------
               jumpmu_return;
            }
            // -------------------------------------------------------------------------------------
            {
               MutableSlice primary_payload = iterator.mutableValue();
               auto& primary_version = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
               // -------------------------------------------------------------------------------------
               // Checks
               ensure(primary_version.worker_id == cr::Worker::my().workerID());
               ensure(primary_version.worker_commit_mark == cr::activeTX().TTS());
               ensure(!primary_version.isWriteLocked());
               // -------------------------------------------------------------------------------------
               primary_version.writeLock();
               undo_sn = primary_version.next_sn;
               iterator.markAsDirty();
            }
            {
               btree.setSN(m_key, undo_sn);
               ret = iterator.seekExactWithHint(key, true);
               ensure(ret == OP_RESULT::OK);
               secondary_payload_length = iterator.value().length();
               std::memcpy(secondary_payload, iterator.value().data(), secondary_payload_length);
               iterator.markAsDirty();
            }
            {
               btree.setSN(m_key, 0);
               ret = iterator.seekExactWithHint(key, false);
               ensure(ret == OP_RESULT::OK);
               MutableSlice primary_payload = iterator.mutableValue();
               auto& primary_version = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
               const auto& secondary_version = *reinterpret_cast<ChainedTupleVersion*>(secondary_payload);
               primary_version.next_sn = secondary_version.next_sn;
               primary_version.worker_commit_mark = secondary_version.worker_commit_mark;
               primary_version.worker_id = secondary_version.worker_id;
               // -------------------------------------------------------------------------------------
               const auto& update_descriptor = *reinterpret_cast<const UpdateSameSizeInPlaceDescriptor*>(secondary_version.payload);
               BTreeLL::applyDiff(update_descriptor, primary_version.payload, secondary_version.payload + update_descriptor.size());
               // -------------------------------------------------------------------------------------
               if (cr::activeTX().isSerializable() && FLAGS_2pl) {
                  primary_version.read_lock_counter &= ~(1ull << cr::Worker::my().workerID());
               }
               // -------------------------------------------------------------------------------------
               primary_version.unlock();
               iterator.markAsDirty();  // TODO: write CLS
            }
            // TODO: atm, we don't delete the undo_sn to keep the chain for the readers intact
            // TODO: we can instead let the readers restart the chain when the first secondary_sn is not found
            if (0) {
               btree.setSN(m_key, undo_sn);
               ret = iterator.seekExactWithHint(key, true);
               ensure(ret == OP_RESULT::OK);
               ret = iterator.removeCurrent();
               ensure(ret == OP_RESULT::OK);
               iterator.markAsDirty();
               iterator.mergeIfNeeded();
            }
         }
         jumpmuCatch() { UNREACHABLE(); }
         break;
      }
      case WAL_LOG_TYPE::WALRemove: {
         auto& remove_entry = *reinterpret_cast<const WALRemove*>(&entry);
         const u16 key_length = remove_entry.key_length + sizeof(ChainSN);
         u8 key_buffer[key_length];
         std::memcpy(key_buffer, remove_entry.payload, remove_entry.key_length);
         Slice key(key_buffer, key_length);
         MutableSlice m_key(key_buffer, key_length);
         // -------------------------------------------------------------------------------------
         jumpmuTry()
         {
            ChainSN secondary_sn, undo_next_sn;
            OP_RESULT ret;
            BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
            u16 removed_value_length;
            u8 removed_value[PAGE_SIZE];
            u8 undo_worker_id;
            u64 undo_tts;
            // -------------------------------------------------------------------------------------
            {
               btree.setSN(m_key, 0);
               ret = iterator.seekExact(key);
               ensure(ret == OP_RESULT::OK);
               auto& primary_version = *reinterpret_cast<ChainedTuple*>(iterator.mutableValue().data());
               secondary_sn = primary_version.next_sn;
               if (primary_version.worker_id != cr::Worker::my().workerID()) {
                  raise(SIGTRAP);
               }
               ensure(primary_version.worker_id == cr::Worker::my().workerID());
               ensure(primary_version.worker_commit_mark == cr::activeTX().TTS());
               primary_version.writeLock();
            }
            // -------------------------------------------------------------------------------------
            {
               btree.setSN(m_key, secondary_sn);
               ret = iterator.seekExactWithHint(key, true);
               ensure(ret == OP_RESULT::OK);
               auto secondary_payload = iterator.value();
               auto const secondary_version = *reinterpret_cast<const ChainedTupleVersion*>(secondary_payload.data());
               removed_value_length = secondary_payload.length() - sizeof(ChainedTupleVersion);
               std::memcpy(removed_value, secondary_version.payload, removed_value_length);
               undo_worker_id = secondary_version.worker_id;
               undo_tts = secondary_version.worker_commit_mark;
               undo_next_sn = secondary_version.next_sn;
               iterator.markAsDirty();
            }
            // -------------------------------------------------------------------------------------
            {
               btree.setSN(m_key, 0);
               ret = iterator.seekExactWithHint(key, 0);
               ensure(ret == OP_RESULT::OK);
               // Resize
               const u16 new_primary_payload_length = removed_value_length + sizeof(ChainedTuple);
               const Slice old_primary_payload = iterator.value();
               if (old_primary_payload.length() < new_primary_payload_length) {
                  iterator.extendPayload(new_primary_payload_length);
               } else {
                  iterator.shorten(new_primary_payload_length);
               }
               auto primary_payload = iterator.mutableValue();
               auto& primary_version = *new (primary_payload.data()) ChainedTuple(undo_worker_id, undo_tts);
               std::memcpy(primary_version.payload, removed_value, removed_value_length);
               primary_version.next_sn = undo_next_sn;
               ensure(primary_version.is_removed == false);
               primary_version.unlock();
               iterator.markAsDirty();
            }
            // -------------------------------------------------------------------------------------
            {
               btree.setSN(m_key, secondary_sn);
               ret = iterator.seekExactWithHint(key, true);
               ensure(ret == OP_RESULT::OK);
               ret = iterator.removeCurrent();
               ensure(ret == OP_RESULT::OK);
               iterator.markAsDirty();
            }
         }
         jumpmuCatch() { UNREACHABLE(); }
         break;
      }
      default: {
         break;
      }
   }
}
// -------------------------------------------------------------------------------------
void BTreeVI::precisePageWiseGarbageCollection(HybridPageGuard<BTreeNode>& c_guard)
{
   bool all_tuples_heads_are_invisible = true;  // WRT scanners
   u32 garbage_seen_in_bytes = 0;
   u32 freed_bytes = 0;
   for (u16 s_i = 0; s_i < c_guard->count;) {
      auto& sn = *reinterpret_cast<ChainSN*>(c_guard->getKey(s_i) + c_guard->getKeyLen(s_i) - sizeof(ChainSN));
      if (sn == 0) {
         auto& tuple = *reinterpret_cast<Tuple*>(c_guard->getPayload(s_i));
         if (tuple.tuple_format == TupleFormat::CHAINED) {
            auto& chained_tuple = *reinterpret_cast<ChainedTuple*>(c_guard->getPayload(s_i));
            if (chained_tuple.is_removed) {
               all_tuples_heads_are_invisible &= (isVisibleForMe(tuple.worker_id, tuple.worker_commit_mark));
               const u32 size = c_guard->getKVConsumedSpace(s_i);
               garbage_seen_in_bytes += size;
               if (chained_tuple.worker_commit_mark <= cr::Worker::my().global_snapshot_lwm) {
                  c_guard->removeSlot(s_i);
                  freed_bytes += size;
               } else {
                  s_i++;
               }
            } else {
               all_tuples_heads_are_invisible &= !(isVisibleForMe(tuple.worker_id, tuple.worker_commit_mark));
               s_i++;
            }
         } else if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
            // TODO: Fix FatTuple size
            all_tuples_heads_are_invisible &= !(isVisibleForMe(tuple.worker_id, tuple.worker_commit_mark));
            s_i++;
         }
      } else {
         auto& chained_tuple_version = *reinterpret_cast<ChainedTupleVersion*>(c_guard->getPayload(s_i));
         const u32 size = c_guard->getKVConsumedSpace(s_i);
         if (chained_tuple_version.gc_trigger <= cr::Worker::my().global_snapshot_lwm) {
            c_guard->removeSlot(s_i);
            freed_bytes += size;
         } else {
            garbage_seen_in_bytes += size;
            s_i++;
         }
      }
   }
   c_guard->gc_space_used = garbage_seen_in_bytes;
   // -------------------------------------------------------------------------------------
   const bool have_we_modified_the_page = (freed_bytes > 0) || (all_tuples_heads_are_invisible);
   if (have_we_modified_the_page) {
      c_guard.incrementGSN();
   }
   // -------------------------------------------------------------------------------------
   if (all_tuples_heads_are_invisible) {
      c_guard->skip_if_gsn_equal = c_guard.bf->page.GSN;
      c_guard->and_if_your_sat_older = cr::Worker::my().snapshotAcquistionTime();
   }
}
// -------------------------------------------------------------------------------------
SpaceCheckResult BTreeVI::checkSpaceUtilization(void* btree_object, BufferFrame& bf)
{
   [[maybe_unused]] auto& btree = *reinterpret_cast<BTreeGeneric*>(btree_object);
   Guard bf_guard(bf.header.latch);
   bf_guard.toOptimisticOrJump();
   HybridPageGuard<BTreeNode> c_guard(std::move(bf_guard), &bf);
   if (!c_guard->is_leaf || !triggerPageWiseGarbageCollection(c_guard)) {
      return SpaceCheckResult::NOTHING;
   }
   // -------------------------------------------------------------------------------------
   bool has_removed_anything = false;
   for (u16 s_i = 0; s_i < c_guard->count;) {
      auto& sn = *reinterpret_cast<ChainSN*>(c_guard->getKey(s_i) + c_guard->getKeyLen(s_i) - sizeof(ChainSN));
      if (sn == 0) {
         auto& tuple = *reinterpret_cast<Tuple*>(c_guard->getPayload(s_i));
         if (tuple.tuple_format == TupleFormat::CHAINED) {
            auto& chained_tuple = *reinterpret_cast<ChainedTuple*>(c_guard->getPayload(s_i));
            if (chained_tuple.is_removed && chained_tuple.worker_commit_mark <= cr::Worker::my().global_snapshot_lwm) {
               if (!has_removed_anything) {
                  has_removed_anything = true;
                  c_guard.toExclusive();
               }
               c_guard->removeSlot(s_i);
            } else {
               s_i++;
            }
         } else if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
            // TODO: Fix FatTuple size
            s_i++;
         }
      } else {
         auto& chained_tuple_version = *reinterpret_cast<ChainedTupleVersion*>(c_guard->getPayload(s_i));
         if (chained_tuple_version.gc_trigger <= cr::Worker::my().global_snapshot_lwm) {
            if (!has_removed_anything) {
               has_removed_anything = true;
               c_guard.toExclusive();
            }
            c_guard->removeSlot(s_i);
         } else {
            s_i++;
         }
      }
   }
   if (has_removed_anything) {
      return SpaceCheckResult::RETRY_SAME_BF;
   } else {
      return SpaceCheckResult::NOTHING;
   }
   // TODO: return BTreeGeneric::checkSpaceUtilization(static_cast<BTreeGeneric*>(&btree), bf);
}
// -------------------------------------------------------------------------------------
void BTreeVI::todo(void* btree_object, const u8* entry_ptr, const u64 version_worker_id, const u64 version_tts)
{
   auto& btree = *reinterpret_cast<BTreeVI*>(btree_object);
   // Only point-gc
   const TODOPoint& point_todo = *reinterpret_cast<const TODOPoint*>(entry_ptr);
   if (FLAGS_vi_dangling_pointer && point_todo.dangling_pointer.isValid()) {
      // Optimistic fast path
      jumpmuTry()
      {
         BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree), point_todo.dangling_pointer.bf,
                                         point_todo.dangling_pointer.latch_version_should_be);
         assert(point_todo.dangling_pointer.bf != nullptr);
         auto& node = iterator.leaf;
         auto& head = *reinterpret_cast<ChainedTuple*>(node->getPayload(point_todo.dangling_pointer.head_slot));
         // Being chained is implicit because we check for version, so the state can not be changed after staging the todo
         ensure(head.tuple_format == TupleFormat::CHAINED && !head.isWriteLocked());
         ensure(head.worker_id == version_worker_id && head.worker_commit_mark == version_tts);
         if (head.is_removed) {
            iterator.leaf->gc_space_used -= iterator.leaf->getKVConsumedSpace(point_todo.dangling_pointer.secondary_slot);
            iterator.leaf->gc_space_used -= iterator.leaf->getKVConsumedSpace(point_todo.dangling_pointer.head_slot);
            node->removeSlot(point_todo.dangling_pointer.secondary_slot);
            node->removeSlot(point_todo.dangling_pointer.head_slot);
         } else {
            head.reset();
            head.next_sn = reinterpret_cast<ChainedTupleVersion*>(node->getPayload(point_todo.dangling_pointer.secondary_slot))->next_sn;
            iterator.leaf->gc_space_used -= iterator.leaf->getKVConsumedSpace(point_todo.dangling_pointer.secondary_slot);
            node->removeSlot(point_todo.dangling_pointer.secondary_slot);
         }
         iterator.mergeIfNeeded();
         jumpmu_return;
      }
      jumpmuCatch() {}
   }
   // -------------------------------------------------------------------------------------
   const u16 key_length = point_todo.key_length + sizeof(ChainSN);
   u8 key_buffer[key_length];
   std::memcpy(key_buffer, point_todo.key, point_todo.key_length);
   MutableSlice m_key(key_buffer, key_length);
   Slice key(key_buffer, key_length);
   btree.setSN(m_key, 0);
   OP_RESULT ret;
   // -------------------------------------------------------------------------------------
   jumpmuTry()
   {
      BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
      ret = iterator.seekExact(key);
      if (ret != OP_RESULT::OK) {  // Legit case
         jumpmu_return;
      }
      COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_todo_chains[btree.dt_id]++; }
      // -------------------------------------------------------------------------------------
      MutableSlice primary_payload = iterator.mutableValue();
      {
         // Checks
         const auto& tuple = *reinterpret_cast<const Tuple*>(primary_payload.data());
         if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
            jumpmu_return;
         }
      }
      // -------------------------------------------------------------------------------------
      ChainedTuple& primary_version = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
      if (!primary_version.isWriteLocked()) {
         ChainSN remove_next_sn = 0;
         bool next_sn_higher = true;
         if (primary_version.worker_id == version_worker_id && primary_version.worker_commit_mark == version_tts) {
            remove_next_sn = primary_version.next_sn;
            // Main version is visible we can prune the whole chain
            if (primary_version.is_removed) {
               iterator.leaf->gc_space_used -= iterator.leaf->getKVConsumedSpace(iterator.cur);
               ret = iterator.removeCurrent();
               ensure(ret == OP_RESULT::OK);
               iterator.mergeIfNeeded();
               COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_todo_remove[btree.dt_id]++; }
            } else {
               primary_version.reset();
               primary_version.next_sn = 0;
               COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_todo_updates[btree.dt_id]++; }
            }
         } else {
            // Search for the first visible-to-all version
            ChainSN search_next_sn = primary_version.next_sn;
            while (search_next_sn != 0) {
               next_sn_higher = search_next_sn >= btree.getSN(key);
               btree.setSN(m_key, search_next_sn);
               ret = iterator.seekExactWithHint(key, next_sn_higher);
               if (ret != OP_RESULT::OK) {
                  break;
               }
               // -------------------------------------------------------------------------------------
               MutableSlice secondary_payload = iterator.mutableValue();
               auto& secondary_version = *reinterpret_cast<ChainedTupleVersion*>(secondary_payload.data());
               if (cr::Worker::my().isVisibleForAll(secondary_version.committed_before_sat) ||
                   (secondary_version.worker_id == version_worker_id && secondary_version.worker_commit_mark == version_tts)) {
                  remove_next_sn = secondary_version.next_sn;
                  secondary_version.next_sn = 0;
                  break;
               } else {
                  search_next_sn = secondary_version.next_sn;
               }
            }
         }
         while (remove_next_sn != 0) {
            next_sn_higher = remove_next_sn >= btree.getSN(key);
            btree.setSN(m_key, remove_next_sn);
            ret = iterator.seekExactWithHint(key, next_sn_higher);
            if (ret != OP_RESULT::OK) {
               // raise(SIGTRAP); should be fine if the tuple got converted to fat
               break;
            }
            // -------------------------------------------------------------------------------------
            Slice secondary_payload = iterator.value();
            const auto& secondary_version = *reinterpret_cast<const ChainedTupleVersion*>(secondary_payload.data());
            remove_next_sn = secondary_version.next_sn;
            // -------------------------------------------------------------------------------------
            iterator.leaf->gc_space_used -= iterator.leaf->getKVConsumedSpace(iterator.cur);
            iterator.removeCurrent();
            iterator.mergeIfNeeded();
            iterator.markAsDirty();
            COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_todo_versions_removed[btree.dt_id]++; }
         }
      }
   }
   jumpmuCatch() { UNREACHABLE(); }
}
// -------------------------------------------------------------------------------------
void BTreeVI::unlock(void* btree_object, const u8* entry_ptr)
{
   auto& btree = *reinterpret_cast<BTreeVI*>(btree_object);
   const auto& todo_entry = *reinterpret_cast<const UnlockEntry*>(entry_ptr);
   // -------------------------------------------------------------------------------------
   const u16 key_length = todo_entry.key_length;
   u8 key_buffer[key_length];
   std::memcpy(key_buffer, todo_entry.key, todo_entry.key_length);
   MutableSlice m_key(key_buffer, key_length);
   Slice key(key_buffer, key_length);
   btree.setSN(m_key, 0);
   OP_RESULT ret;
   // -------------------------------------------------------------------------------------
   jumpmuTry()
   {
      BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
      ret = iterator.seekExact(key);
      ensure(ret == OP_RESULT::OK);
      // -------------------------------------------------------------------------------------
      MutableSlice primary_payload = iterator.mutableValue();
      Tuple& primary_version = *reinterpret_cast<Tuple*>(primary_payload.data());
      primary_version.read_lock_counter &= ~(1ull << cr::Worker::my().workerID());
   }
   jumpmuCatch() { UNREACHABLE(); }
}
// -------------------------------------------------------------------------------------
struct DTRegistry::DTMeta BTreeVI::getMeta()
{
   DTRegistry::DTMeta btree_meta = {.iterate_children = iterateChildrenSwips,
                                    .find_parent = findParent,
                                    .check_space_utilization = checkSpaceUtilization,
                                    .checkpoint = checkpoint,
                                    .undo = undo,
                                    .todo = todo,
                                    .unlock = unlock,
                                    .serialize = serialize,
                                    .deserialize = deserialize};
   return btree_meta;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::scanDesc(u8* o_key, u16 o_key_length, function<bool(const u8*, u16, const u8*, u16)> callback, function<void()>)
{
   return scan<false>(o_key, o_key_length, callback);
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::scanAsc(u8* o_key,
                           u16 o_key_length,
                           function<bool(const u8* key, u16 key_length, const u8* value, u16 value_length)> callback,
                           function<void()>)
{
   return scan<true>(o_key, o_key_length, callback);
}
// -------------------------------------------------------------------------------------
std::tuple<OP_RESULT, u16> BTreeVI::reconstructChainedTuple(BTreeSharedIterator& iterator,
                                                            MutableSlice key,
                                                            std::function<void(Slice value)> callback)
{
   assert(getSN(key) == 0);
   u16 chain_length = 1;
   OP_RESULT ret;
   u16 materialized_value_length;
   std::unique_ptr<u8[]> materialized_value;
   ChainSN secondary_sn;
   {
      Slice primary_payload = iterator.value();
      const ChainedTuple& primary_version = *reinterpret_cast<const ChainedTuple*>(primary_payload.data());
      if (isVisibleForMe(primary_version.worker_id, primary_version.worker_commit_mark, false)) {
         if (primary_version.is_removed) {
            return {OP_RESULT::NOT_FOUND, 1};
         }
         callback(Slice(primary_version.payload, primary_payload.length() - sizeof(ChainedTuple)));
         return {OP_RESULT::OK, 1};
      }
      if (primary_version.isFinal()) {
         return {OP_RESULT::NOT_FOUND, 1};
      }
      materialized_value_length = primary_payload.length() - sizeof(ChainedTuple);
      materialized_value = std::make_unique<u8[]>(materialized_value_length);
      std::memcpy(materialized_value.get(), primary_version.payload, materialized_value_length);
      secondary_sn = primary_version.next_sn;
   }
   // -------------------------------------------------------------------------------------
   bool next_sn_higher = true;
   while (secondary_sn != 0) {
      setSN(key, secondary_sn);
      ret = iterator.seekExactWithHint(Slice(key.data(), key.length()), next_sn_higher);
      if (ret != OP_RESULT::OK) {
         explainWhen(dt_id == 0);
         return {OP_RESULT::NOT_FOUND, chain_length};
      }
      chain_length++;
      ensure(chain_length <= FLAGS_vi_max_chain_length);
      Slice payload = iterator.value();
      const auto& secondary_version = *reinterpret_cast<const ChainedTupleVersion*>(payload.data());
      if (secondary_version.is_delta) {
         // Apply delta
         const auto& update_descriptor = *reinterpret_cast<const UpdateSameSizeInPlaceDescriptor*>(secondary_version.payload);
         BTreeLL::applyDiff(update_descriptor, materialized_value.get(), secondary_version.payload + update_descriptor.size());
      } else {
         materialized_value_length = payload.length() - sizeof(ChainedTupleVersion);
         materialized_value = std::make_unique<u8[]>(materialized_value_length);
         std::memcpy(materialized_value.get(), secondary_version.payload, materialized_value_length);
      }
      ensure(!secondary_version.is_removed);
      if (isVisibleForMe(secondary_version.worker_id, secondary_version.worker_commit_mark, false)) {
         if (secondary_version.is_removed) {
            return {OP_RESULT::NOT_FOUND, chain_length};
         }
         callback(Slice(materialized_value.get(), materialized_value_length));
         return {OP_RESULT::OK, chain_length};
      }
      if (secondary_version.isFinal()) {
         return {OP_RESULT::NOT_FOUND, chain_length};
      } else {
         next_sn_higher = secondary_version.next_sn >= secondary_sn;
         secondary_sn = secondary_version.next_sn;
      }
   }
   return {OP_RESULT::NOT_FOUND, chain_length};
}
// -------------------------------------------------------------------------------------
}  // namespace btree
}  // namespace storage
}  // namespace leanstore
