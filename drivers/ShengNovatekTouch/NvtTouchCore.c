// SPDX-License-Identifier: Apache-2.0
// See NvtTouchCore.h and NOTICE. Simplified finger-only reimplementation.
#include "NvtTouchCore.h"
#include <string.h>

static const int kRowMap[NVT_ROWS] = {
    26, 78, 130, 182, 234, 286, 338, 390, 442, 494,
    546, 598, 650, 702, 754, 806, 856, 906, 956, 1006,
    1056, 1106, 1156, 1206, 1256, 1306, 1356, 1406, 1456, 1506,
    1556, 1606, 1656, 1706, 1756, 1806, 1856, 1906, 1956, 2006,
};
static const int kColumnMap[NVT_COLUMNS] = {
    25, 75, 125, 175, 225, 275, 325, 375, 425, 476,
    528, 580, 632, 684, 736, 788, 840, 892, 944, 996,
    1048, 1099, 1149, 1199, 1249, 1299, 1349, 1399, 1449, 1499,
    1549, 1599, 1649, 1699, 1749, 1799, 1849, 1899, 1949, 2000,
    2052, 2104, 2156, 2208, 2260, 2312, 2364, 2416, 2468, 2520,
    2572, 2623, 2673, 2723, 2773, 2823, 2873, 2923, 2973, 3023,
};
#define AXIS_BASE 2700
#define AXIS_EDGE 600
#define AXIS_GAIN 4350
#define MAX_PEAKS 24
#define TRACK_RADIUS (NVT_SUPER_RESOLUTION*50*4)   // 2000 units: about 20% of the short axis per frame is far

int nvt_touch_row_center(int row) {return row>=0 && row<NVT_ROWS ? kRowMap[row] : 0;}
int nvt_touch_column_center(int column) {return column>=0 && column<NVT_COLUMNS ? kColumnMap[column] : 0;}

int nvt_touch_map_edge_axis(int value, int maximum, int low_edge_bias) {
    if (value < AXIS_EDGE) {
        int mapped = AXIS_GAIN * (AXIS_EDGE - value) / AXIS_BASE;
        if (mapped > AXIS_EDGE - 1) mapped = AXIS_EDGE - 1;
        return AXIS_EDGE - mapped - (low_edge_bias ? 1 : 0);
    }
    int edge_start = maximum - AXIS_EDGE;
    if (value > edge_start) {
        int mapped = AXIS_GAIN * (value - edge_start) / AXIS_BASE;
        if (mapped > AXIS_EDGE - 1) mapped = AXIS_EDGE - 1;
        return edge_start + mapped;
    }
    return value;
}

void nvt_touch_reset(struct nvt_touch_core *core) {
    memset(core, 0, sizeof(*core));
}

// The lowest identifier no active slot holds, 1..NVT_SLOTS. The identifier is the contact's path index upstream, and backboardd drops
// every path whose index reaches its path limit (int-267: after about 33 strokes no touch reached SpringBoard), so identifiers are
// reused once their finger is up instead of counting up for the whole boot.
static int free_tracking_id(const struct nvt_touch_core *core) {
    for (int id=1;id<=NVT_SLOTS;++id) {
        int taken=0;
        for (int s=0;s<NVT_SLOTS && !taken;++s) taken=core->slots[s].active && core->slots[s].tracking_id==id;
        if (!taken) return id;
    }
    return NVT_SLOTS;
}

// Smoothing factor (1/65536) of a first-order low-pass filter with this cutoff, sampled once a frame: w / (1 + w), w = 2 pi fc / rate.
static long filter_alpha(long cutoff_mhz) {
    const long w=cutoff_mhz*411775/(1000L*NVT_FRAME_HZ);   // 2 pi * 65536 = 411775
    return w*65536/(65536+w);
}

// The reported position of a tracked finger for its raw one: an adaptive low-pass filter whose cutoff follows the finger's smoothed
// speed (NvtTouchCore.h). It is continuous: no movement is held back and released in one step.
// A frame whose blob is below the down threshold (a finger lifting off, or barely touching) moves nothing: its centroid is the least
// reliable, and the last frames before a lift otherwise give the lift-off a velocity it did not have.
static void follow(struct nvt_slot *slot, int x, int y, int weak) {
    if (weak) return;
    const long sx=((long)x*16-slot->fx)*NVT_FRAME_HZ/16,sy=((long)y*16-slot->fy)*NVT_FRAME_HZ/16;
    const long speed_alpha=filter_alpha(NVT_SPEED_CUTOFF_MHZ);
    slot->vx+=(sx-slot->vx)*speed_alpha/65536;slot->vy+=(sy-slot->vy)*speed_alpha/65536;
    const long speed=(slot->vx<0?-slot->vx:slot->vx)+(slot->vy<0?-slot->vy:slot->vy);
    const long alpha=filter_alpha(NVT_MIN_CUTOFF_MHZ+speed*NVT_CUTOFF_PER_SPEED/2);
    slot->fx+=((long)x*16-slot->fx)*alpha/65536;slot->fy+=((long)y*16-slot->fy)*alpha/65536;
    slot->rx=(int)((slot->fx+8)/16);slot->ry=(int)((slot->fy+8)/16);
}

// The reason a tracked finger at (x, y) matched none of this frame's blobs, judged by the nearest blob of any kind.
static int miss_reason(const struct nvt_slot *slot, const struct nvt_contact *found, const int *used, int nfound,
                       const int (*palms)[2], int npalms) {
    long best=-1;int reason=NVT_MISS_NONE;
    for (int c=0;c<nfound;++c) {
        const long dx=found[c].x-slot->x,dy=found[c].y-slot->y,d=dx*dx+dy*dy;
        if (best>=0 && d>=best) continue;
        best=d;reason=used[c] ? NVT_MISS_TAKEN : found[c].peak<NVT_UP_THRESHOLD ? NVT_MISS_WEAK : NVT_MISS_FAR;
    }
    for (int p=0;p<npalms;++p) {
        const long dx=palms[p][0]-slot->x,dy=palms[p][1]-slot->y,d=dx*dx+dy*dy;
        if (best<0 || d<best) {best=d;reason=NVT_MISS_PALM;}
    }
    if (best>(long)TRACK_RADIUS*TRACK_RADIUS && reason!=NVT_MISS_NONE) reason=NVT_MISS_FAR;
    return reason;
}

// The cost of giving a finger blob j (j >= nfound: no blob): the squared distance from where the finger is heading, the radius
// squared for no blob, more than that for a blob out of reach or too weak to keep a finger.
static long track_cost(const struct nvt_slot *slot, const struct nvt_contact *found, int nfound, int j) {
    const long r2=(long)TRACK_RADIUS*TRACK_RADIUS;
    if (j>=nfound) return r2;
    if (found[j].peak<NVT_UP_THRESHOLD) return 4*r2;
    const long px=slot->x+(long)slot->dx*(slot->missed+1),py=slot->y+(long)slot->dy*(slot->missed+1);
    const long dx=found[j].x-px,dy=found[j].y-py,d=dx*dx+dy*dy;
    return d<=r2 ? d : 4*r2;
}

// Least-cost assignment of the active fingers (rows) to the blobs plus one "no blob" column per finger (Hungarian method with
// potentials, O(rows^2 * columns)); match[slot] gets the blob or -1.
static void assign_tracks(const struct nvt_touch_core *core, const struct nvt_contact *found, int nfound, int *match) {
    enum {COLUMNS=MAX_PEAKS+NVT_SLOTS};
    int rows[NVT_SLOTS+1],n=0;
    for (int s=0;s<NVT_SLOTS;++s) {match[s]=-1;if (core->slots[s].active) rows[++n]=s;}
    if (!n) return;
    const int m=nfound+n;
    long u[NVT_SLOTS+1]={0},v[COLUMNS+1]={0},minv[COLUMNS+1];
    int p[COLUMNS+1]={0},way[COLUMNS+1]={0};char used[COLUMNS+1];
    for (int i=1;i<=n;++i) {
        p[0]=i;int j0=0;
        for (int j=0;j<=m;++j) {minv[j]=__LONG_MAX__;used[j]=0;}
        do {
            used[j0]=1;
            const int i0=p[j0];long delta=__LONG_MAX__;int j1=0;
            for (int j=1;j<=m;++j) {
                if (used[j]) continue;
                const long cur=track_cost(&core->slots[rows[i0]],found,nfound,j-1)-u[i0]-v[j];
                if (cur<minv[j]) {minv[j]=cur;way[j]=j0;}
                if (minv[j]<delta) {delta=minv[j];j1=j;}
            }
            for (int j=0;j<=m;++j) {
                if (used[j]) {u[p[j]]+=delta;v[j]-=delta;}
                else minv[j]-=delta;
            }
            j0=j1;
        } while (p[j0]);
        do {const int j1=way[j0];p[j0]=p[j1];j0=j1;} while (j0);
    }
    const long r2=(long)TRACK_RADIUS*TRACK_RADIUS;
    for (int j=1;j<=nfound;++j) {
        if (!p[j]) continue;
        const int s=rows[p[j]];
        if (track_cost(&core->slots[s],found,nfound,j-1)<=r2) match[s]=j-1;
    }
}

static void sort_i16(int16_t *v, unsigned n) {
    for (unsigned i=1;i<n;++i) {int16_t x=v[i];unsigned j=i;while(j>0 && v[j-1]>x){v[j]=v[j-1];--j;}v[j]=x;}
}

static void form_reference(struct nvt_touch_core *core) {
    int16_t column[NVT_STARTUP_FRAMES];
    for (unsigned node=0;node<NVT_NODES;++node) {
        for (unsigned s=0;s<NVT_STARTUP_FRAMES;++s) column[s]=core->startup[s][node];
        sort_i16(column,NVT_STARTUP_FRAMES);
        core->reference[node]=(int16_t)(((int)column[NVT_STARTUP_FRAMES/2-1]+(int)column[NVT_STARTUP_FRAMES/2])/2);
    }
    core->have_reference=1;core->startup_count=0;
}

struct peak {int index, value;};
struct blob {long sum_w, sum_row, sum_col; int peak, area, label;};

static int delta_at(const int16_t *m, const int16_t *ref, int index) {return (int)m[index]-(int)ref[index];}

int nvt_touch_process(struct nvt_touch_core *core, const int16_t *matrix, unsigned frame_type,
                      struct nvt_contact *out, int max) {
    if (frame_type != 2 && frame_type != 4) return 0;
    if (frame_type == 2 && !core->have_reference) {
        memcpy(core->reference, matrix, sizeof(core->reference));
        core->have_reference=1;core->startup_count=0;return -1;
    }
    if (!core->have_reference) {
        memcpy(core->startup[core->startup_count++], matrix, sizeof(core->reference));
        if (core->startup_count < NVT_STARTUP_FRAMES) return -1;
        form_reference(core);
        return -1;
    }
    ++core->frames_by_type[frame_type==4];
    int *delta=core->delta;
    for (int i=0;i<NVT_NODES;++i) delta[i]=delta_at(matrix,core->reference,i);
    // Slow reference drift: in a frame without a finger the quiet nodes move the reference a step towards their value (NvtTouchCore.h).
    {
        int touched=0;
        for (int i=0;i<NVT_NODES && !touched;++i) touched=delta[i]>=NVT_PEAK_THRESHOLD;
        if (!touched) {
            for (int i=0;i<NVT_NODES;++i) {
                const int step=delta[i]/NVT_REFERENCE_RATE;
                core->reference[i]=(int16_t)(core->reference[i]+(step ? step : delta[i]>0 ? 1 : delta[i]<0 ? -1 : 0));
            }
            ++core->reference_frames;
        }
    }

    // Steepest-ascent labelling: every node above the noise floor flows to the local maximum above it.
    int *label=core->label;
    struct peak peaks[MAX_PEAKS];int npeaks=0,overflow=0;
    int replaced[MAX_PEAKS];
    for (int i=0;i<NVT_NODES;++i) label[i]=-1;
    for (int i=0;i<NVT_NODES;++i) {
        if (delta[i] < NVT_NOISE_THRESHOLD) continue;
        int cur=i;
        for (;;) {
            int best=cur,bv=delta[cur],r=cur/NVT_COLUMNS,c=cur%NVT_COLUMNS;
            for (int dr=-1;dr<=1;++dr) for (int dc=-1;dc<=1;++dc) {
                if (!dr && !dc) continue;
                int rr=r+dr,cc=c+dc;
                if (rr<0||rr>=NVT_ROWS||cc<0||cc>=NVT_COLUMNS) continue;
                int n=rr*NVT_COLUMNS+cc;
                if (delta[n] > bv || (delta[n]==bv && n>best && delta[n]>delta[cur])) {bv=delta[n];best=n;}
            }
            if (best==cur) break;
            cur=best;
        }
        // cur is a local maximum; register it as a peak the first time we see it. A full table drops its weakest maximum for a
        // stronger one (the nodes already labelled with it are cleared below), so a finger is never lost to a crowd of faint maxima.
        int found=-1;
        for (int p=0;p<npeaks;++p) if (peaks[p].index==cur) {found=p;break;}
        if (found<0) {
            if (npeaks==MAX_PEAKS) {
                overflow=1;
                int weakest=0;
                for (int p=1;p<npeaks;++p) if (peaks[p].value<peaks[weakest].value) weakest=p;
                if (peaks[weakest].value>=delta[cur]) continue;
                peaks[weakest].index=cur;peaks[weakest].value=delta[cur];++replaced[weakest];found=weakest;
            } else {
                replaced[npeaks]=0;peaks[npeaks].index=cur;peaks[npeaks].value=delta[cur];found=npeaks++;
            }
        }
        label[i]=found | replaced[found]<<8;
    }
    if (overflow) ++core->peak_overflows;
    if ((unsigned long)npeaks>core->peaks_max) core->peaks_max=(unsigned long)npeaks;
    // A label names its peak and the peak's generation: nodes that flowed to a dropped maximum belong to nothing.
    for (int i=0;i<NVT_NODES;++i) if (label[i]>=0) label[i]=(label[i]>>8)==replaced[label[i]&0xff] ? (label[i]&0xff) : -1;
    // One finger can show two maxima a node or two apart (a ridge across it). Left as two blobs, the finger's centroid jumps by half a
    // node whenever the ridge moves; a peak with a stronger one within NVT_MERGE_SPAN nodes is folded into it.
    int into[MAX_PEAKS];
    for (int p=0;p<npeaks;++p) {
        into[p]=p;
        int pr=peaks[p].index/NVT_COLUMNS,pc=peaks[p].index%NVT_COLUMNS;
        for (int q=0;q<npeaks;++q) {
            if (q==p) continue;
            int dr=peaks[q].index/NVT_COLUMNS-pr,dc=peaks[q].index%NVT_COLUMNS-pc;
            if (dr<-NVT_MERGE_SPAN||dr>NVT_MERGE_SPAN||dc<-NVT_MERGE_SPAN||dc>NVT_MERGE_SPAN) continue;
            int stronger=peaks[q].value>peaks[p].value || (peaks[q].value==peaks[p].value && q<p);
            if (stronger && (into[p]==p || peaks[q].value>peaks[into[p]].value)) into[p]=q;
        }
    }
    for (int p=0;p<npeaks;++p) {
        int r=p;
        while (into[r]!=r) r=into[r];   // values rise strictly along the chain, so it ends
        if (r!=p) ++core->merges;
        into[p]=r;
    }
    for (int i=0;i<NVT_NODES;++i) if (label[i]>=0) label[i]=into[label[i]];
    struct blob blobs[MAX_PEAKS];
    for (int p=0;p<npeaks;++p) memset(&blobs[p],0,sizeof(blobs[p])),blobs[p].peak=peaks[p].value,blobs[p].label=p;
    for (int i=0;i<NVT_NODES;++i) {
        if (label[i]<0) continue;
        struct blob *b=&blobs[label[i]];
        int floor=peaks[label[i]].value*NVT_CENTROID_FLOOR/100;if (floor<NVT_COORDINATE_REDUCE) floor=NVT_COORDINATE_REDUCE;
        int w=delta[i]-floor;if (w<0) w=0;
        b->sum_w+=w;b->sum_row+=(long)w*kRowMap[i/NVT_COLUMNS]*NVT_SUPER_RESOLUTION;
        b->sum_col+=(long)w*kColumnMap[i%NVT_COLUMNS]*NVT_SUPER_RESOLUTION;++b->area;
    }
    struct nvt_contact found[MAX_PEAKS];int nfound=0;
    int palms[MAX_PEAKS][2];int npalms=0;
    for (int p=0;p<npeaks;++p) {
        struct blob *b=&blobs[p];
        if (!b->sum_w) continue;
        // Starting a contact needs the down threshold, keeping one only the up threshold.
        int x=(int)((b->sum_col + b->sum_w/2) / b->sum_w);
        int y=(int)((b->sum_row + b->sum_w/2) / b->sum_w);
        if (x<0) x=0; if (x>NVT_LONG_AXIS_MAX-1) x=NVT_LONG_AXIS_MAX-1;
        if (y<0) y=0; if (y>NVT_SHORT_AXIS_MAX-1) y=NVT_SHORT_AXIS_MAX-1;
        x=nvt_touch_map_edge_axis(x,NVT_LONG_AXIS_MAX,0);
        y=nvt_touch_map_edge_axis(y,NVT_SHORT_AXIS_MAX,1);
        if (b->area>=NVT_PALM_MIN_NODES) {palms[npalms][0]=x;palms[npalms][1]=y;++npalms;continue;}   // palm-sized, not reported
        found[nfound].slot=-1;found[nfound].tracking_id=0;found[nfound].x=x;found[nfound].y=y;
        found[nfound].peak=b->peak;found[nfound].area=b->area;found[nfound].weight=(int)b->sum_w;++nfound;
    }
    // Track: the assignment of blobs (above the up threshold, within TRACK_RADIUS of each finger's predicted position) to fingers with
    // the least total squared distance; a finger may also take no blob, at the cost of the radius. Matching slot by slot let a finger
    // take its neighbour's blob (int-275: 5 strokes ended "taken"), and so does taking the closest pair first when two fingers move
    // together; measuring from the last position instead of the predicted one ended fast swipes (13 of 125 strokes ended "far").
    int used_contact[MAX_PEAKS]={0},match[NVT_SLOTS];
    assign_tracks(core,found,nfound,match);
    for (int s=0;s<NVT_SLOTS;++s) if (match[s]>=0) used_contact[match[s]]=1;
    for (int s=0;s<NVT_SLOTS;++s) {
        struct nvt_slot *slot=&core->slots[s];
        if (!slot->active) continue;
        const int best=match[s];
        if (best<0) {
            // A finger is not lifted by one frame without it (int-269: every stroke ended after 60..130 ms, so a swipe never paused
            // and the app switcher never opened); it is held where it was for up to NVT_HOLD_FRAMES frames.
            if (!slot->missed) slot->miss_reason=miss_reason(slot,found,used_contact,nfound,(const int (*)[2])palms,npalms);
            if (++slot->missed<=NVT_HOLD_FRAMES) continue;
            ++core->ends[slot->miss_reason];slot->active=0;continue;
        }
        const int dx=found[best].x-slot->x,dy=found[best].y-slot->y,ax=dx<0?-dx:dx,ay=dy<0?-dy:dy;
        slot->dx=dx/(slot->missed+1);slot->dy=dy/(slot->missed+1);
        if (slot->missed) {++core->holds_bridged;slot->missed=0;}
        found[best].slot=s;found[best].tracking_id=slot->tracking_id;
        const int px=slot->rx,py=slot->ry;
        slot->x=found[best].x;slot->y=found[best].y;++slot->age;
        follow(slot,found[best].x,found[best].y,found[best].peak<NVT_DOWN_THRESHOLD);
        found[best].x=slot->rx;found[best].y=slot->ry;
        if (ax<4*NVT_FUZZ && ay<4*NVT_FUZZ) {
            const int ox=slot->rx>px?slot->rx-px:px-slot->rx,oy=slot->ry>py?slot->ry-py:py-slot->ry;
            ++core->jitter_frames;core->jitter_sum+=(unsigned long)(ax+ay);core->smoothed_sum+=(unsigned long)(ox+oy);
            if (ax>core->jitter_max) core->jitter_max=ax;
            if (ay>core->jitter_max) core->jitter_max=ay;
            if (ox>core->smoothed_max) core->smoothed_max=ox;
            if (oy>core->smoothed_max) core->smoothed_max=oy;
        }
    }
    for (int c=0;c<nfound;++c) {
        if (used_contact[c] || found[c].peak<NVT_DOWN_THRESHOLD) continue;
        for (int s=0;s<NVT_SLOTS;++s) {
            if (core->slots[s].active) continue;
            core->slots[s].active=1;core->slots[s].tracking_id=free_tracking_id(core);core->slots[s].age=1;core->slots[s].missed=0;
            core->slots[s].x=found[c].x;core->slots[s].y=found[c].y;
            core->slots[s].rx=found[c].x;core->slots[s].ry=found[c].y;
            core->slots[s].fx=(long)found[c].x*16;core->slots[s].fy=(long)found[c].y*16;core->slots[s].vx=core->slots[s].vy=0;
            core->slots[s].dx=core->slots[s].dy=0;
            found[c].slot=s;found[c].tracking_id=core->slots[s].tracking_id;used_contact[c]=1;break;
        }
    }
    int count=0;
    for (int c=0;c<nfound && count<max;++c) {
        if (found[c].slot<0 || core->slots[found[c].slot].age<2) continue;   // report from the second frame on
        out[count++]=found[c];
    }
    for (int s=0;s<NVT_SLOTS && count<max;++s) {   // held fingers, where they were last reported
        const struct nvt_slot *slot=&core->slots[s];
        if (!slot->active || !slot->missed || slot->age<2) continue;
        out[count].slot=s;out[count].tracking_id=slot->tracking_id;out[count].x=slot->rx;out[count].y=slot->ry;
        out[count].peak=out[count].area=out[count].weight=0;++count;
    }
    return count;
}
