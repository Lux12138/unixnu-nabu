// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../RootPublication.h"
struct Operations {
  bool allocated[4]={}, attached[4]={}, started[4]={};
  int failPrepare=-1, failStep=-1, failStart=-1, failWait=-1;
  bool valid=true, scheduleOk=true;
  unsigned schedules=0;
  std::vector<unsigned> startOrder, waitOrder, discarded;
  bool prepare(unsigned index) {
    // Distinct failures before allocation, after allocation, and after attach.
    if(int(index)==failPrepare && failStep==0) return false;
    allocated[index]=true;
    if(int(index)==failPrepare && failStep==1) return false;
    attached[index]=true;
    return !(int(index)==failPrepare && failStep==2);
  }
  bool validate() { return valid; }
  bool schedule() { ++schedules; return scheduleOk; }
  void discard(unsigned index) {
    assert(!started[index]);
    allocated[index]=attached[index]=false;discarded.push_back(index);
  }
  bool start(unsigned index) {
    assert(allocated[index] && attached[index] && !started[index]);
    startOrder.push_back(index);
    if(int(index)==failStart) return false;
    started[index]=true;return true;
  }
  bool wait(unsigned index) { assert(started[index]);waitOrder.push_back(index);return int(index)!=failWait; }
  bool empty() const { for(bool x:allocated)if(x)return false;return true; }
};
struct Iterator {
  std::vector<int *> entries;
  size_t position=0;
  bool valid=true, invalidateOnNext=false;
  bool isValid() {return valid;}
  int *getNextObject() {if(invalidateOnNext)valid=false;return position<entries.size()?entries[position++]:nullptr;}
};
int main() {
  const char *names[] = {"OS","Aux","SystemOS","Other"}; UfsRootPlan plan;
  assert(ufsBuildRootPlan(names,4,plan));
  assert(plan.count==4 && plan.order[0]==2 && plan.order[1]==1 && plan.order[2]==3 && plan.order[3]==0);
  const char *bad[][4]={{"OS","Other"},{"SystemOS","Other"},{"OS","OS"},{"OS",nullptr},{"OS",""}};
  for(auto &n:bad) {UfsRootPlan p;assert(!ufsBuildRootPlan(n,2,p) && p.count==0);}
  for(unsigned count:{0U,1U,5U}) {UfsRootPlan p;assert(!ufsBuildRootPlan(names,count,p));}
  // Every preparation failure position and allocation/attachment phase must
  // free the current partial object and all prior ones, without scheduling.
  for(int index=0;index<4;++index)for(int step=0;step<3;++step) {
    Operations ops;ops.failPrepare=index;ops.failStep=step;
    auto r=ufsPrepareRootWindows(ops,4);
    assert(r.phase==UfsRootPhase::Prepare && r.index==unsigned(index));
    assert(ops.empty() && !ops.schedules && ops.discarded.size()==unsigned(index+1));
  }
  for(bool identity:{false,true}) {
    Operations ops;ops.valid=identity;ops.scheduleOk=false;
    auto r=ufsPrepareRootWindows(ops,4);
    assert(r.phase==(identity?UfsRootPhase::Schedule:UfsRootPhase::Identity));
    assert(ops.empty() && ops.schedules==unsigned(identity));
  }
  {Operations ops;assert(ufsPrepareRootWindows(ops,4).phase==UfsRootPhase::Complete);
    auto r=ufsPublishRootWindows(ops,plan);assert(r.phase==UfsRootPhase::Complete && r.published==4);
    assert((ops.startOrder==std::vector<unsigned>{2,1,3,0}) && ops.startOrder==ops.waitOrder && ops.discarded.empty());}
  // No diagnostic flag enters the coordinator: reporting cannot relax gates.
  for(unsigned position=0;position<4;++position)for(bool waitFailure:{false,true}) {
    Operations ops;assert(ufsPrepareRootWindows(ops,4).phase==UfsRootPhase::Complete);
    unsigned index=plan.order[position];if(waitFailure)ops.failWait=index;else ops.failStart=index;
    auto r=ufsPublishRootWindows(ops,plan);
    assert(r.index==index && r.phase==(waitFailure?UfsRootPhase::Media:UfsRootPhase::Start));
    assert(ops.startOrder.size()==position+1 && r.published==position+unsigned(waitFailure));
    for(unsigned p=0;p<4;++p) {
      unsigned i=plan.order[p];bool keep=p<position || (p==position && waitFailure);
      assert(ops.allocated[i]==keep && ops.started[i]==keep && ops.attached[i]==keep);
    }
  }
  int window=0,wrongWindow=0,media=0;
  {Iterator i{{&media,&wrongWindow}};assert(!ufsMediaHasAncestor(i,&window));}
  {Iterator i{{&media,&window}};assert(ufsMediaHasAncestor(i,&window));}
  {Iterator i{{&window}};i.valid=false;assert(!ufsMediaHasAncestor(i,&window));}
  {Iterator i{{&window}};i.invalidateOnNext=true;assert(!ufsMediaHasAncestor(i,&window));}
  {Iterator i; i.entries.assign(256,&media);i.entries.push_back(&window);assert(!ufsMediaHasAncestor(i,&window));}
  puts("PASS: complete preparation or rollback, dispatch failure, SystemOS first/OS last, every start/media failure, no future publication, retained started devices, bounded valid ancestry");
}
