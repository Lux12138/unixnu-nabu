/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_VM_ALIAS_H
#define NATIVE_ADRENO_VM_ALIAS_H
#include <stddef.h>
#include <stdint.h>

/* Temporary CPU validation index, never a GPU mapping or translation table.
 * Range has physical/bytes fields. The caller has validated nonempty ranges
 * with nonoverflowing ends and supplies count valid indices in owned scratch.
 * Original ranges are immutable. One serialized owner uses scratch at a time. */
template<class Range> class NativeAdrenoPhysicalIndex final {
    const Range *ranges_;
    uint32_t *order_;
    size_t count_;
    uint64_t end(uint32_t i) const {return ranges_[i].physical+ranges_[i].bytes;}
    bool less(uint32_t a,uint32_t b) const {
        return ranges_[a].physical<ranges_[b].physical ||
            (ranges_[a].physical==ranges_[b].physical && ranges_[a].bytes<ranges_[b].bytes);
    }
    void sift(size_t root,size_t count) {
        while(root<count/2) {
            size_t child=root*2+1;
            if(child+1<count && less(order_[child],order_[child+1]))++child;
            if(!less(order_[root],order_[child]))break;
            uint32_t t=order_[root];order_[root]=order_[child];order_[child]=t;root=child;
        }
    }
public:
    NativeAdrenoPhysicalIndex(const Range *ranges,uint32_t *order,size_t count,bool sorted=false):ranges_(ranges),order_(order),count_(count) {
        if(sorted) {
            return;
        }
        for(size_t i=count/2;i-->0;)sift(i,count);
        for(size_t n=count;n>1;) {--n;uint32_t t=order_[0];order_[0]=order_[n];order_[n]=t;sift(0,n);}
        /* Discard an interval contained by an earlier interval. The remaining
         * starts AND ends are monotonic, even for nested/duplicate/overlapping
         * inputs. Each query then needs a binary search, with no backward scan. */
        size_t kept=0;
        for(size_t i=0;i<count;++i) {
            uint32_t index=order_[i];
            if(!kept || end(index)>end(order_[kept-1]))order_[kept++]=index;
        }
        count_=kept;
    }
    size_t count() const {
        return count_;
    }
    bool overlaps(uint64_t physical,uint64_t bytes) const {
        size_t lo=0,hi=count_;
        while(lo<hi) {size_t mid=lo+(hi-lo)/2;if(end(order_[mid])<=physical)lo=mid+1;else hi=mid;}
        return lo<count_ && ranges_[order_[lo]].physical<physical+bytes;
    }
    bool conflictingCache(uint64_t physical,uint64_t bytes,unsigned writeback) const {
        size_t lo=0,hi=count_;
        while(lo<hi) {
            const size_t mid=lo+(hi-lo)/2;
            if(end(order_[mid])<=physical) {
                lo=mid+1;
            } else {
                hi=mid;
            }
        }
        for(size_t at=lo;at<count_ && ranges_[order_[at]].physical<physical+bytes;++at) {
            if(ranges_[order_[at]].writeback!=writeback) {
                return true;
            }
        }
        return false;
    }
};
#endif
