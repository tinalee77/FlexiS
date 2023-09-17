/*
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the 
 * Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY 
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>. 
 */

#include <linux/module.h>
#include <net/tcp.h>
#include <net/tcp_states.h>
#include <linux/list.h>
#include <linux/time64.h>

// the minimum number of slopes needed for trend analysis
static int sigma __read_mostly = 3;
module_param(sigma, int, 0644);
// the first increase factor
static int alpha __read_mostly = 100;
module_param(alpha, int, 0644);
// the second increase factor
static int beta __read_mostly = 10;
module_param(beta, int, 0644);
// the decrease factor magnified by 100 times
static int gamma __read_mostly = 85;
module_param(gamma, int, 0644);
// the minimum duration in ms required for trend analysis
static int tau __read_mostly = 60;
module_param(tau, int, 0644);
// the threshold for the slope (magnified 1000 times) of the fit line
static int theta __read_mostly = 30;
module_param(theta, int, 0644);

#define MAX_U32 0xffffffff
#define MAX_RTT MAX_U32
#define MIN_CWND 2U

/*
 * returning the median of values between start and end. the values should be sorted
 * @head: pointing to the head of a list of values
 * @start: the index of the first valid list entry 
 * @end: the index of the last valid list entry 
 * @type: the type of list entries
 * @member: the name of the member of a list entry
 */
#define median(head, start, end, type, member) ({ \
		struct list_head *ptr, *head__ = (head); \
		u32 start__ = (start), end__ = (end), pos = 0, pos_mid; \
		type *ptr1, *ptr2; \
		typeof(((type *)0)->member) res; \
		pos_mid = (start__ + end__) >> 1U; \
		list_for_each(ptr, head__) { \
			pos++; \
			if (pos == pos_mid) { \
				ptr1 = container_of(ptr, type, links); \
				ptr2 = container_of(ptr->next, type, links); \
				break; \
			} \
		} \
		if ((start__ + end__) % 2) { \
			res = (ptr1->member + ptr2->member) / 2; \
		} else { \
			res = ptr1->member; \
		} \
		res; \
})
/*
 * adding a new entry to a list sorted in ascending order
 * @head: head of the list
 * @node: pointing to the entry to be added to the list
 * @member: the name of the member used as sorting key
 */
#define add_asd(head, node, member) ({ \
		bool added = false; \
		struct list_head *head__ = (head); \
		typeof(node) node__ = (node), ptr; \
		if (!list_empty(head__)) { \
			list_for_each_entry(ptr, head__, links) { \
			if (node__->member < ptr->member) { \
				list_add_tail(&node__->links, &ptr->links); \
				added = true; \
				break; \
			} \
		} \
		} \
		if (!added) \
		list_add_tail(&node__->links, head__); \
})

// return code definition 
enum ret {
	SUCCESS,
	NULL_PTR,
	ZERO_DIV,
	OUT_RNG,
	EMPTY_QUE,
	FULL_QUE,
	NO_MEM
};

// the rtt sample struct
struct rnode {
	u32 rtt_us;
	struct list_head links;
};
/*
 * used for rtt sample compression
 * @head: the head of a list of rnodes that have the same snd_time_ms
 * @snd_time_ms: the sending time of the data packet that is used to estimate the rtt
 * @cnt: the number of rtt samples in the list
 */ 
struct rtt_bin {
	struct list_head head; 
	u64 snd_time_ms; 
	u32 cnt;
};
// the slope struct
struct snode {
	s32 slope;
	struct list_head links;    
};
/* 
 * a list that stores the slopes of lines connecting pairs of points in the rtt_sack. sorted in ascending order
 * @head: pointing to the head of a list of snodes
 * @cnt: the number of slopes in the list
 */
struct slopes {
	struct list_head head;
	u32 cnt;
};
// a struct that stores a pointer pointing to an snode in slopes
struct fnode {
	struct snode *ptr;
	struct list_head links;
};
/*
 * struct that stores a data point in the rtt_sack
 * @snd_time_ms: the sending time of the data packets
 * @rtt_us: the median rtt measured by all data packets that are sent at "snd_time_ms"
 * @fanout_head: the head of a list of fnodes, each of which points to an entry in slopes 
 * these pointers are used to quickly remove snodes from slopes when a pnode is removed from rtt_sack
 */ 
struct pnode {
	u64 snd_time_ms; 
	u32 rtt_us; 
	struct list_head fanout_head;  
	struct list_head links; 
};
/*
 * the RTT SACK
 * @head: the head of a list of pnodes
 * @cnt: the number of pnodes in rtt_sack
 */ 
struct rtt_sack {
	struct list_head head; 
	u32 cnt;
};
/*
 * the rest of variables of the flexis struct
 * @t0: start time of an increase epoch
 * @r0: the initial rate at the start of an increase epoch
 * @t_ulmt: the time when flexis enters cwnd unlimited state
 * @snd_nxt: a copy of tcp's snd_nxt before cwnd is decreased. no trend analysis is done before snd_nxt is acknowledged. 
 * it is used to prevent multiple cwnd reduction within one window. 
 * it also makes sure that the first trend analysis after cwnd reduction is based on the new cwnd.
 * @undo_cwnd: used by tcp to undo its wrong cwnd reduction
 * @rtt_us: the latest rtt sample in us
 * @epoch_min_rtt: the minimum rtt observed in the pending phase and the following increase phase
 */ 
struct vars {
	u64 t0; 
	u64 t_ulmt; 
	u32 r0; 
	u32 snd_nxt; 
	u32 undo_cwnd;  
	s32 rtt_us; 
	u32 epoch_min_rtt; 
};
/*
 * The flexis struct
 * @rtt_bin: storing rtt samples with the same sending time in ascending order
 * @rtt_sack: the rtt sack
 * @slopes: storing slopes (magnified 1000 times) of lines connecting pairs of points in rtt_sack
 * the rest of variables are placed in vars due to size limitation
 */
struct flexis {
	struct rtt_bin rtt_bin;
	struct rtt_sack rtt_sack; 
	struct slopes slopes; 
	struct vars *vars; 
};

/////////////// rtt_bin operations ///////////////////

// adding one entry to rtt_bin and preserving its ascending order
static int rtt_bin_add_asd(struct sock *sk, u64 snd_time_ms, u32 rtt_us)
{
	struct flexis *flexis = inet_csk_ca(sk);
	struct rnode *rnode = NULL;

	if (flexis->rtt_bin.cnt >= MAX_U32) {
		return FULL_QUE;
	}

	rnode = kzalloc(sizeof(struct rnode), GFP_KERNEL);
	if (unlikely(!rnode)) {
		return NO_MEM;
	}

	rnode->rtt_us = rtt_us;
	INIT_LIST_HEAD(&rnode->links);

	if (list_empty(&flexis->rtt_bin.head)) {
		flexis->rtt_bin.snd_time_ms = snd_time_ms;
	}

	add_asd(&flexis->rtt_bin.head, rnode, rtt_us);

	flexis->rtt_bin.cnt++;

	return SUCCESS;
}

// finding the median of rtt samples stored in rtt_bin
static int rtt_bin_median(struct sock *sk, u32 *mrtt_us) 
{
	struct flexis *flexis = inet_csk_ca(sk);

	if (!mrtt_us) {
		return NULL_PTR;
	}

	if (list_empty(&flexis->rtt_bin.head)) {
		return EMPTY_QUE;
	}

	*mrtt_us = median(&flexis->rtt_bin.head, 1, flexis->rtt_bin.cnt, struct rnode, rtt_us);

	return SUCCESS;
}

// resetting rtt_bin
static void rtt_bin_reset(struct sock *sk)
{
	struct flexis *flexis = inet_csk_ca(sk);
	struct rnode *rnode, *tmp;

	if (!list_empty(&flexis->rtt_bin.head)) {       
		list_for_each_entry_safe(rnode, tmp, &flexis->rtt_bin.head, links) {
			list_del(&rnode->links);
			kfree(rnode);
		}
	}

	INIT_LIST_HEAD(&flexis->rtt_bin.head);
	flexis->rtt_bin.snd_time_ms = 0;
	flexis->rtt_bin.cnt = 0;
}

///////////// slope operations ////////////

// adding a new slope into slopes and preserving the ascending order
static struct snode *slopes_add_asd(struct sock *sk, struct slopes *slopes, s32 slope)
{
	struct snode *snode;

	if (slopes->cnt >= MAX_U32) {
		return NULL;
	}

	snode = kzalloc(sizeof(struct snode), GFP_KERNEL);
	if (unlikely(!snode)) {
		return NULL;
	}

	snode->slope = slope;
	INIT_LIST_HEAD(&snode->links);

	add_asd(&slopes->head, snode, slope);

	slopes->cnt++;

	return snode;
}

// adding a list of sorted slopes to slopes and preserving ascending order
static int slopes_add_asd_mul(struct sock *sk, struct slopes *new_slopes)
{
	struct flexis *flexis = inet_csk_ca(sk);
	struct snode *snode_new, *snode_old, *tmp;

	if (list_empty(&new_slopes->head)) {
		return EMPTY_QUE;
	}

	if (list_empty(&flexis->slopes.head)) {
		list_splice_tail(&new_slopes->head, &flexis->slopes.head);
		flexis->slopes.cnt = new_slopes->cnt;
		return SUCCESS;
	}

	snode_old = list_first_entry(&flexis->slopes.head, struct snode, links);
	list_for_each_entry_safe(snode_new, tmp, &new_slopes->head, links) {
		list_for_each_entry_from(snode_old, &flexis->slopes.head, links) { 
			if (snode_new->slope < snode_old->slope) { 
				list_del(&snode_new->links);
				new_slopes->cnt--;
				list_add_tail(&snode_new->links, &snode_old->links); 
				flexis->slopes.cnt++;
				break;
			} 
		} 
	} 
	if (!list_empty(&new_slopes->head)) { 
		list_splice_tail(&new_slopes->head, &flexis->slopes.head);
		flexis->slopes.cnt += new_slopes->cnt;
	}

	return SUCCESS;
}

// adding a new node to the end of the fanout queue of a pnode
static int fanout_enq(struct sock *sk, struct list_head *fanout_head, struct snode *snode)
{
	struct fnode *fnode;

	if (!fanout_head || !snode) {
		return NULL_PTR;
	}

	fnode = kzalloc(sizeof(struct fnode), GFP_KERNEL);
	if (unlikely(!fnode)) {
		return NO_MEM;
	}

	fnode->ptr = snode;
	INIT_LIST_HEAD(&fnode->links);

	list_add_tail(&fnode->links, fanout_head);

	return SUCCESS;
}

// calculating slopes of lines connecting each old pnode and the newly added pnode in rtt_sack 
static int slopes_gen(struct sock *sk, struct pnode *stop_pnode)
{
	struct flexis *flexis = inet_csk_ca(sk);
	struct pnode *pnode;
	struct snode *snode;
	struct slopes new_slopes;
	s32 diff, slope;

	if (!stop_pnode) {
		return NULL_PTR;
	}

	if (list_empty(&flexis->rtt_sack.head)) {
		return EMPTY_QUE;
	}

	INIT_LIST_HEAD(&new_slopes.head);
	new_slopes.cnt = 0;

	list_for_each_entry(pnode, &flexis->rtt_sack.head, links) {
		if (pnode == stop_pnode)
			break;
		diff = stop_pnode->snd_time_ms - pnode->snd_time_ms;
		if (diff > 0) {
			// the slope is magnified 1000 times
			slope = (s32)(stop_pnode->rtt_us - pnode->rtt_us) / diff; 
			snode = slopes_add_asd(sk, &new_slopes, slope);
			if (!snode) {
				return NO_MEM;
			}
			fanout_enq(sk, &pnode->fanout_head, snode);
		}
	}
	return slopes_add_asd_mul(sk, &new_slopes);
}

// returning the median of slopes
static int slopes_median(struct sock *sk, u32 start, u32 end, s32 *mslope) 
{
	struct flexis *flexis = inet_csk_ca(sk);

	if (!mslope) {
		return NULL_PTR;
	}

	if (start < 1 || end > flexis->slopes.cnt) {
		return OUT_RNG;
	}

	if (list_empty(&flexis->slopes.head)) {
		return EMPTY_QUE;
	}

	*mslope = median(&flexis->slopes.head, start, end, struct snode, slope);

	return SUCCESS;
}

// removing the specified slope from slopes
static int slopes_del(struct sock *sk, struct snode *snode)
{
	struct flexis *flexis = inet_csk_ca(sk);

	if (!snode) {
		return NULL_PTR;
	}

	if (list_empty(&flexis->slopes.head)) {
		return EMPTY_QUE;
	}

	list_del(&snode->links);
	kfree(snode);

	flexis->slopes.cnt--;

	return SUCCESS;
}

// resetting the fanout queue
static int fout_reset(struct sock *sk, struct list_head *fanout_head)
{
	struct fnode *fnode, *tmp;

	if (!fanout_head) {
		return NULL_PTR;
	}

	if (list_empty(fanout_head)) {
		return EMPTY_QUE;
	}

	list_for_each_entry_safe(fnode, tmp, fanout_head, links) {
		slopes_del(sk, fnode->ptr);
		list_del(&fnode->links);
		kfree(fnode);
	} 

	INIT_LIST_HEAD(fanout_head);

	return SUCCESS;
}

// resetting slopes
static void slopes_reset(struct sock *sk)
{
	struct flexis *flexis = inet_csk_ca(sk);
	struct snode *snode, *tmp;

	if (!list_empty(&flexis->slopes.head)) {
		list_for_each_entry_safe(snode, tmp, &flexis->slopes.head, links) {
			list_del(&snode->links);
			kfree(snode);
		}
	}

	INIT_LIST_HEAD(&flexis->slopes.head);
	flexis->slopes.cnt = 0; 
}

///////// rtt_sack operations //////////////

// adding a new pnode to rtt_sack
static struct pnode * rtt_sack_enq(struct sock *sk, u64 snd_time_ms, u32 rtt_us)
{
	struct flexis *flexis = inet_csk_ca(sk);
	struct pnode *new_node;

	if (flexis->rtt_sack.cnt >= MAX_U32) {
		return NULL;
	}

	new_node = kzalloc(sizeof(struct pnode), GFP_KERNEL);
	if (unlikely(!new_node)) {
		return NULL;
	}

	new_node->snd_time_ms = snd_time_ms;
	new_node->rtt_us = rtt_us;
	INIT_LIST_HEAD(&new_node->fanout_head);
	INIT_LIST_HEAD(&new_node->links);
	list_add_tail(&new_node->links, &flexis->rtt_sack.head);

	flexis->rtt_sack.cnt++;

	return new_node;
}

// removing the oldest pnode from rtt_sack
static void rtt_sack_deq(struct sock *sk)
{
	struct flexis *flexis = inet_csk_ca(sk);
	struct pnode *fst_tnode;

	if (!list_empty(&flexis->rtt_sack.head)) {
		fst_tnode = list_first_entry(&flexis->rtt_sack.head, struct pnode, links);
		if (fst_tnode) {
			if (!list_empty(&fst_tnode->fanout_head)) {
				fout_reset(sk, &fst_tnode->fanout_head);
			}
			list_del(&fst_tnode->links);
			kfree(fst_tnode);
		}
		flexis->rtt_sack.cnt--;
	}
}

// resetting the rtt_sack
static void rtt_sack_reset(struct sock *sk)
{
	struct flexis *flexis = inet_csk_ca(sk);

	while (!list_empty(&flexis->rtt_sack.head)) {  
		rtt_sack_deq(sk);
	}

	INIT_LIST_HEAD(&flexis->rtt_sack.head);
	flexis->rtt_sack.cnt = 0;
}

//////////// other helper operations /////////////

static bool is_cwnd_limited(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	return tp->snd_cwnd < 2 * tp->max_packets_out;
}

// initializing increase epoch
static void init_inc_epoch(struct sock *sk)
{
	struct flexis *flexis = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	
	if (flexis->vars->epoch_min_rtt)
		flexis->vars->r0 = div_u64(tp->snd_cwnd * USEC_PER_SEC, flexis->vars->epoch_min_rtt);
	else if (tp->srtt_us)
		flexis->vars->r0 = div_u64(tp->snd_cwnd * USEC_PER_SEC, tp->srtt_us >> 3);
	else 
		flexis->vars->r0 /= 2;
	
	flexis->vars->t0 = tp->tcp_mstamp;

}

static void update_pacing_ratio(struct sock *sk, u32 pr)
{
	if (pr)
		sock_net(sk)->ipv4.sysctl_tcp_pacing_ss_ratio = sock_net(sk)->ipv4.sysctl_tcp_pacing_ca_ratio = pr;

}

static void increase_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct flexis *flexis = inet_csk_ca(sk);
	long t1, t2;
	u64 r1, r2, rem;
	u32 srtt, pr, dur;

	if (!flexis->vars->t0) { 
		return;
	}
	
	if (!is_cwnd_limited(sk)) {
		update_pacing_ratio(sk, 100);
		if (!flexis->vars->t_ulmt) {
			flexis->vars->t_ulmt = tp->tcp_mstamp;
                }
		return;
	}
	if (!alpha || !beta )
		return;
	
	srtt = tp->srtt_us >> 3;
	if (!srtt) {
		return;
	}
	
	// right shift t0 after cwnd unlimited period to resume rate increase smoothly
	if (flexis->vars->t_ulmt) {
		dur = tp->tcp_mstamp - flexis->vars->t_ulmt;
		if (dur > 0) {
			flexis->vars->t0 += dur;
		}
		flexis->vars->t_ulmt = 0;
	}
	
	// t1 is the current elapsed time
	t1 = tp->tcp_mstamp - flexis->vars->t0;
	if (t1 < 0) {
		return;
	}
	
	// r1 is the current rate, its unit is packets per second
	r1 = int_pow(t1 / USEC_PER_MSEC / alpha, 3) + t1 / USEC_PER_MSEC / beta  + flexis->vars->r0;
	if (!r1)
		return;
	
	if (flexis->vars->epoch_min_rtt) {
		tp->snd_cwnd = max(tp->snd_cwnd, min_t(u32, div_u64(r1 * flexis->vars->epoch_min_rtt, (u32)USEC_PER_SEC), tp->snd_cwnd_clamp));
	} else {
		tp->snd_cwnd = max(tp->snd_cwnd, min_t(u32, div_u64(r1 * srtt, (u32)USEC_PER_SEC), tp->snd_cwnd_clamp));
	}
	
	flexis->vars->undo_cwnd = tp->snd_cwnd;
	
	// t2 is the elapsed time in one rtt 
	if (flexis->vars->epoch_min_rtt) {
		t2 = t1 + flexis->vars->epoch_min_rtt;
	} else {
		t2 = t1 + srtt;
	}
	// r2 is the rate in one rtt
	r2 = int_pow(t2 / USEC_PER_MSEC / alpha, 3) + t2 / USEC_PER_MSEC / beta + flexis->vars->r0;
	// calculating pacing ratio pr
	pr = div64_u64_rem(r2 * 100, r1, &rem);
	if (rem)
		pr++;
	update_pacing_ratio(sk, pr);
}

static void decrease_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct flexis *flexis = inet_csk_ca(sk);
	
	tp->snd_cwnd = min(tp->snd_cwnd, max_t(u32, div_u64((u64)tp->snd_cwnd * gamma, 100), MIN_CWND));

	flexis->vars->undo_cwnd = tp->snd_cwnd;
}

// reinitializing data structures after cwnd reduction
static void reinit_after_dec(struct sock *sk)
{
	struct flexis *flexis = inet_csk_ca(sk);

	rtt_bin_reset(sk);
	rtt_sack_reset(sk);
	slopes_reset(sk);
	flexis->vars->t0 = 0;
	flexis->vars->epoch_min_rtt = MAX_RTT;
	flexis->vars->snd_nxt = 0;
	flexis->vars->t_ulmt = 0;
	update_pacing_ratio(sk, 100);
}

/////////////// system operations ////////////////

static void tcp_flexis_init(struct sock *sk)
{
	struct flexis *flexis = inet_csk_ca(sk);	
	struct tcp_sock *tp = tcp_sk(sk);

	flexis->vars = kzalloc(sizeof(struct vars), GFP_KERNEL);
	if (unlikely(!flexis->vars)) {
		return;
	} 
	
	flexis->vars->t0 = 0;
	flexis->vars->r0 = 0;
	flexis->vars->t_ulmt = 0;
	flexis->vars->undo_cwnd = tp->snd_cwnd;
	flexis->vars->snd_nxt = 0;
	flexis->vars->rtt_us = -1;
	flexis->vars->epoch_min_rtt = MAX_RTT;
	INIT_LIST_HEAD(&flexis->rtt_bin.head);
	flexis->rtt_bin.snd_time_ms = 0;
	flexis->rtt_bin.cnt = 0;
	INIT_LIST_HEAD(&flexis->rtt_sack.head);
	flexis->rtt_sack.cnt = 0;
	INIT_LIST_HEAD(&flexis->slopes.head);
	flexis->slopes.cnt = 0;
	cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
	update_pacing_ratio(sk, 100);
}

u32 tcp_flexis_ssthresh(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 thr;
	
	thr = max(tp->snd_cwnd >> 1, MIN_CWND);
	
	return thr;
}

u32 tcp_flexis_undo_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct flexis *flexis = inet_csk_ca(sk);

	if (!flexis->vars) {
		return max(tp->snd_cwnd, tp->prior_cwnd);
	}

	return flexis->vars->undo_cwnd;
}

static void tcp_flexis_cwnd_event(struct sock *sk, enum tcp_ca_event ev)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct flexis *flexis = inet_csk_ca(sk);
	bool reinit = true;

	if (!flexis->vars) {
		return;
	}

	switch (ev) {
	case CA_EVENT_CWND_RESTART: 
		break;
	case CA_EVENT_COMPLETE_CWR: 
		if (flexis->vars->snd_nxt) { 
			// TCP reduced cwnd while flexis was reducing it. We undo cwnd to the value before the second cwnd reduction
			tp->snd_cwnd = flexis->vars->undo_cwnd;
		}
		break;
	case CA_EVENT_LOSS: 
		break;
	default:
		reinit = false;
		break;
	}
	
	if (reinit) {
		reinit_after_dec(sk);
	}
}

static void tcp_flexis_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct flexis *flexis = inet_csk_ca(sk);
	struct pnode *new_pnode;
	u64 snd_time_us, snd_time_ms;
	u32 dur, med_rtt;
	s32 theil_slope;
	bool reasoning = false;
	int rst;

	if (!flexis->vars) {
		return;
	}

	if (flexis->vars->rtt_us == -1) {
		return;
	}

	snd_time_us = max_t(s64, tp->tcp_mstamp - (u64)flexis->vars->rtt_us, 0);
	if (!snd_time_us) {
		return;
	}

	snd_time_ms = div_u64(snd_time_us, (u32)USEC_PER_MSEC);
	if (snd_time_ms < flexis->rtt_bin.snd_time_ms) {
		return;
	}

	if (flexis->vars->snd_nxt) {
		if (ack <= flexis->vars->snd_nxt) { 
			// ignoring rtt samples measured by packets sent before cwnd reduction
			return;
		} else { 
			// the first rtt sample measured by the first packet sent after cwnd reduction has arrived
			reinit_after_dec(sk);
		}
	}
	
	if (flexis->vars->rtt_us < flexis->vars->epoch_min_rtt)
			flexis->vars->epoch_min_rtt = flexis->vars->rtt_us;

	if (snd_time_ms == flexis->rtt_bin.snd_time_ms) {
		// rtt sample compression
		rtt_bin_add_asd(sk, snd_time_ms, flexis->vars->rtt_us);
	} else { 
		if (!list_empty(&flexis->rtt_bin.head)) {
			rst = rtt_bin_median(sk, &med_rtt);
			new_pnode = rtt_sack_enq(sk, flexis->rtt_bin.snd_time_ms, med_rtt);
			if (new_pnode) {
				if (!slopes_gen(sk, new_pnode)) {
					reasoning = true;
				}
			} 
			rtt_bin_reset(sk);
			rtt_bin_add_asd(sk, snd_time_ms, flexis->vars->rtt_us); 
		} else {      
			rtt_bin_add_asd(sk, snd_time_ms, flexis->vars->rtt_us);
		}
	} 

	if (!list_empty(&flexis->rtt_sack.head)) {
		dur = max_t(s64, list_last_entry(&flexis->rtt_sack.head, struct pnode, links)->snd_time_ms - list_first_entry(&flexis->rtt_sack.head, struct pnode, links)->snd_time_ms + 1, 0);
	} else {
		dur = 0;
	}

	// making congestion decision
	if (reasoning && dur >= tau) { 
		if (flexis->rtt_sack.cnt < sigma) {
			goto inc;
		}
		rst = slopes_median(sk, 1, flexis->slopes.cnt, &theil_slope);
		if (theil_slope >= theta) { 
			// congestion detected, decrease cwnd
			flexis->vars->snd_nxt = tp->snd_nxt;
			decrease_cwnd(sk);
			update_pacing_ratio(sk, 100);
			return;
		} 
		inc:
		if (!flexis->vars->t0) {
			init_inc_epoch(sk);
		}
		// removing the oldest pnode from rtt_sack
		rtt_sack_deq(sk);
	}

	// increasing cwnd if allowed
	increase_cwnd(sk);
}

static void tcp_flexis_pkts_acked(struct sock *sk, const struct ack_sample *sample)
{
	struct flexis *flexis = inet_csk_ca(sk);

	if (!flexis->vars) {
		return;
	}

	flexis->vars->rtt_us = sample->rtt_us;
}

static void tcp_flexis_release(struct sock *sk)
{
	struct flexis *flexis = inet_csk_ca(sk);

	if (!flexis->vars) {
		return;
	}

	rtt_bin_reset(sk);
	rtt_sack_reset(sk);
	slopes_reset(sk);
	kfree(flexis->vars);
	flexis->vars = NULL;
}

static struct tcp_congestion_ops tcp_flexis __read_mostly = {
		.init = tcp_flexis_init, 
		.ssthresh = tcp_flexis_ssthresh, 
		.undo_cwnd	= tcp_flexis_undo_cwnd, 
		.cwnd_event = tcp_flexis_cwnd_event, 
		.cong_avoid = tcp_flexis_cong_avoid, 
		.pkts_acked = tcp_flexis_pkts_acked, 
		.release = tcp_flexis_release, 
		.owner = THIS_MODULE,
		.name = "flexis"
};

static int __init tcp_flexis_register(void)
{
	BUILD_BUG_ON(sizeof(struct flexis) > ICSK_CA_PRIV_SIZE);
	return tcp_register_congestion_control(&tcp_flexis);
}

static void __exit tcp_flexis_unregister(void)
{
	tcp_unregister_congestion_control(&tcp_flexis);
}

module_init(tcp_flexis_register);
module_exit(tcp_flexis_unregister);

MODULE_AUTHOR("Qian Li");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP FlexiS");
