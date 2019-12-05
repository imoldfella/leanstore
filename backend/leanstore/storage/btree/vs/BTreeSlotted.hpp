#include "Units.hpp"
#include "Exceptions.hpp"
#include "leanstore/storage/buffer-manager/DTRegistry.hpp"
#include "leanstore/storage/buffer-manager/BufferFrame.hpp"
#include "leanstore/storage/buffer-manager/PageGuard.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
#include <iostream>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cassert>
#include <string>
#include <x86intrin.h>
// -------------------------------------------------------------------------------------
using namespace std;
using namespace leanstore::buffermanager;
// -------------------------------------------------------------------------------------
namespace leanstore {
namespace btree {
namespace vs {
// -------------------------------------------------------------------------------------
struct BTreeNode;
using ValueType = Swip<BTreeNode>;
using SketchType = u32;
// -------------------------------------------------------------------------------------
static inline u64 swap(u64 x) { return __builtin_bswap64(x); }
static inline u32 swap(u32 x) { return __builtin_bswap32(x); }
static inline u16 swap(u16 x) { return __builtin_bswap16(x); }
// -------------------------------------------------------------------------------------
struct BTreeNodeHeader {
   static const unsigned underFullSize = EFFECTIVE_PAGE_SIZE * 0.6;

   struct SeparatorInfo {
      unsigned length;
      unsigned slot;
      bool trunc; // TODO: ???
   };

   struct FenceKey {
      u16 offset;
      u16 length;
   };

   Swip<BTreeNode> upper = nullptr;
   FenceKey lowerFence = {0, 0};
   FenceKey upperFence = {0, 0};

   u16 count = 0;
   bool isLeaf;
   u16 spaceUsed = 0;
   u16 dataOffset = static_cast<u16>(EFFECTIVE_PAGE_SIZE);
   u16 prefixLength = 0;

   static const unsigned hintCount = 16;
   u32 hint[hintCount];

   BTreeNodeHeader(bool isLeaf)
           : isLeaf(isLeaf) {}
   ~BTreeNodeHeader() {}

   inline u8 *ptr() { return reinterpret_cast<u8 *>(this); }
   inline bool isInner() { return !isLeaf; }
   inline u8 *getLowerFenceKey() { return lowerFence.offset ? ptr() + lowerFence.offset : nullptr; }
   inline u8 *getUpperFenceKey() { return upperFence.offset ? ptr() + upperFence.offset : nullptr; }
};
// -------------------------------------------------------------------------------------
struct BTreeNode : public BTreeNodeHeader {
   struct Slot {
      u16 offset;
      u8 headLen;
      u8 restLen;
      union {
         SketchType sketch;
         u8 sketchBytes[4];
      };
   };
   Slot slot[(EFFECTIVE_PAGE_SIZE - sizeof(BTreeNodeHeader)) / (sizeof(Slot))];

   BTreeNode(bool isLeaf)
           : BTreeNodeHeader(isLeaf) {}

   unsigned freeSpace() { return dataOffset - (reinterpret_cast<u8 *>(slot + count) - ptr()); }
   unsigned freeSpaceAfterCompaction() { return EFFECTIVE_PAGE_SIZE - (reinterpret_cast<u8 *>(slot + count) - ptr()) - spaceUsed; }

   bool requestSpaceFor(unsigned spaceNeeded)
   {
      if ( spaceNeeded <= freeSpace())
         return true;
      if ( spaceNeeded <= freeSpaceAfterCompaction()) {
         compactify();
         return true;
      }
      return false;
   }

   // Accessors for normal strings: | Value | restKey | Payload
   inline u8 *getData(unsigned slotId)
   {
      return ptr() + slot[slotId].offset;
   }
   inline u8 *getRest(unsigned slotId)
   {
      assert(!isLarge(slotId));
      return ptr() + slot[slotId].offset + sizeof(ValueType);
   }
   inline unsigned getRestLen(unsigned slotId)
   {
      assert(!isLarge(slotId));
      return slot[slotId].restLen;
   }
   // AAA
   inline u8 *getPayload(unsigned slotId)
   {
      assert(isLeaf);
      assert(!isLarge(slotId));
      return ptr() + slot[slotId].offset + sizeof(ValueType) + getRestLen(slotId);
   }

   // Accessors for large strings: | Value | restLength | restKey | Payload
   static constexpr u8 largeLimit = 254;
   static constexpr u8 largeMarker = largeLimit + 1;
   inline u8 *getRestLarge(unsigned slotId)
   {
      assert(isLarge(slotId));
      return ptr() + slot[slotId].offset + sizeof(ValueType) + sizeof(u16);
   }
   inline u16 &getRestLenLarge(unsigned slotId)
   {
      assert(isLarge(slotId));
      return *reinterpret_cast<u16 *>(ptr() + slot[slotId].offset + sizeof(ValueType));
   }
   inline bool isLarge(unsigned slotId) { return slot[slotId].restLen == largeMarker; }
   inline void setLarge(unsigned slotId) { slot[slotId].restLen = largeMarker; }
   // AAA
   inline u8 *getPayloadLarge(unsigned slotId)
   {
      assert(isLeaf);
      assert(isLarge(slotId));
      return ptr() + slot[slotId].offset + sizeof(ValueType) + sizeof(u16) + getRestLenLarge(slotId);
   }

   // Accessors for both types of strings
   inline u16 getPayloadLength(unsigned slotId) { return *reinterpret_cast<u16 *>(ptr() + slot[slotId].offset); }
   inline ValueType &getValue(unsigned slotId) { return *reinterpret_cast<ValueType *>(ptr() + slot[slotId].offset); }
   inline unsigned getFullKeyLength(unsigned slotId) { return prefixLength + slot[slotId].headLen + (isLarge(slotId) ? getRestLenLarge(slotId) : getRestLen(slotId)); }
   inline void copyFullKey(unsigned slotId, u8 *out, unsigned fullLength)
   {
      memcpy(out, getLowerFenceKey(), prefixLength);
      out += prefixLength;
      fullLength -= prefixLength;
      switch ( slot[slotId].headLen ) {
         case 4:
            *reinterpret_cast<u32 *>(out) = swap(slot[slotId].sketch);
            memcpy(out + slot[slotId].headLen, (isLarge(slotId) ? getRestLarge(slotId) : getRest(slotId)), fullLength - slot[slotId].headLen);
            break;
         case 3:
            out[2] = slot[slotId].sketchBytes[1]; // fallthrough
         case 2:
            out[1] = slot[slotId].sketchBytes[2]; // fallthrough
         case 1:
            out[0] = slot[slotId].sketchBytes[3]; // fallthrough
         case 0:
            break;
         default:
            __builtin_unreachable(); // mmmm, dangerous
      };
   }
   // -------------------------------------------------------------------------------------
   static unsigned spaceNeeded(unsigned keyLength, unsigned prefixLength);
   static int cmpKeys(u8 *a, u8 *b, unsigned aLength, unsigned bLength);
   static SketchType head(u8 *&key, unsigned &keyLength);
   void makeHint();
   // -------------------------------------------------------------------------------------
   template<bool equalityOnly = false>
   s32 lowerBound(u8 *key, unsigned keyLength)
   {
      //for (unsigned i=1; i<count; i++)
      //assert(slot[i-1].sketch <= slot[i].sketch);

      if ( lowerFence.offset )
         assert(cmpKeys(key, getLowerFenceKey(), keyLength, lowerFence.length) > 0);
      if ( upperFence.offset )
         assert(cmpKeys(key, getUpperFenceKey(), keyLength, upperFence.length) <= 0);

      if ( equalityOnly ) {
         if ((keyLength < prefixLength) || (bcmp(key, getLowerFenceKey(), prefixLength) != 0))
            return -1;
      } else {
         int prefixCmp = cmpKeys(key, getLowerFenceKey(), min<unsigned>(keyLength, prefixLength), prefixLength);
         if ( prefixCmp < 0 )
            return 0;
         else if ( prefixCmp > 0 )
            return count;
      }
      key += prefixLength;
      keyLength -= prefixLength;

      unsigned lower = 0;
      unsigned upper = count;

      unsigned oldKeyLength = keyLength;
      SketchType keyHead = head(key, keyLength);

      if ( count > hintCount * 2 ) {
         unsigned dist = count / (hintCount + 1);
         unsigned pos;
         for ( pos = 0; pos < hintCount; pos++ )
            if ( hint[pos] >= keyHead )
               break;
         lower = pos * dist;
         unsigned pos2;
         for ( pos2 = pos; pos2 < hintCount; pos2++ )
            if ( hint[pos2] != keyHead )
               break;
         if ( pos2 < hintCount )
            upper = (pos2 + 1) * dist;
         //cout << isLeaf << " " << count << " " << lower << " " << upper << " " << dist << endl;
      }

      while ( lower < upper ) {
         unsigned mid = ((upper - lower) / 2) + lower;
         if ( keyHead < slot[mid].sketch ) {
            upper = mid;
         } else if ( keyHead > slot[mid].sketch ) {
            lower = mid + 1;
         } else if ( slot[mid].restLen == 0 ) {
            if ( oldKeyLength < slot[mid].headLen ) {
               upper = mid;
            } else if ( oldKeyLength > slot[mid].headLen ) {
               lower = mid + 1;
            } else {
               return mid;
            }
         } else {
            int cmp;
            if ( isLarge(mid)) {
               cmp = cmpKeys(key, getRestLarge(mid), keyLength, getRestLenLarge(mid));
            } else {
               cmp = cmpKeys(key, getRest(mid), keyLength, getRestLen(mid));
            }
            if ( cmp < 0 ) {
               upper = mid;
            } else if ( cmp > 0 ) {
               lower = mid + 1;
            } else {
               return mid;
            }
         }
      }
      if ( equalityOnly )
         return -1;
      return lower;
   }
   // -------------------------------------------------------------------------------------
   void updateHint(unsigned slotId);
   // -------------------------------------------------------------------------------------
   bool insert(u8 *key, unsigned keyLength, ValueType value, u8 *payload = nullptr);
   bool update(u8 *key, unsigned keyLength, u16 payload_length, u8 *payload);
   // -------------------------------------------------------------------------------------
   void compactify();
   // -------------------------------------------------------------------------------------
   // merge right node into this node
   bool merge(unsigned slotId, BTreeNode *parent, BTreeNode *right);
   // store key/value pair at slotId
   void storeKeyValue(u16 slotId, u8 *key, unsigned keyLength, ValueType value, u8 *payload = nullptr);
   void copyKeyValueRange(BTreeNode *dst, u16 dstSlot, u16 srcSlot, unsigned count);
   void copyKeyValue(u16 srcSlot, BTreeNode *dst, u16 dstSlot);
   void insertFence(FenceKey &fk, u8 *key, unsigned keyLength);
   void setFences(u8 *lowerKey, unsigned lowerLen, u8 *upperKey, unsigned upperLen);
   void split(WritePageGuard<BTreeNode> &parent, WritePageGuard<BTreeNode> &new_node, unsigned sepSlot, u8 *sepKey, unsigned sepLength);
   unsigned commonPrefix(unsigned aPos, unsigned bPos);
   SeparatorInfo findSep();
   void getSep(u8 *sepKeyOut, SeparatorInfo info);
   Swip<BTreeNode> &lookupInner(u8 *key, unsigned keyLength);
   // -------------------------------------------------------------------------------------
   // Not synchronized or todo section
   bool removeSlot(unsigned slotId);
   bool remove(u8 *key, unsigned keyLength);
};
// -------------------------------------------------------------------------------------
static_assert(sizeof(BTreeNode) == EFFECTIVE_PAGE_SIZE, "page size problem");
// -------------------------------------------------------------------------------------
}
}
}