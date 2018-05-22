#include "align.h"
#include "primitives.h"
#define NODE_SIZE (1 << 12)
#define N	NODE_SIZE
#define NBITS	(N - 1)
#define BOT ((void *)0)
#define TOP ((void *)-1)

#include <pthread.h>
struct _node_t {
	struct _node_t* volatile next DOUBLE_CACHE_ALIGNED;
	long id DOUBLE_CACHE_ALIGNED;
	void* volatile cells[NODE_SIZE] DOUBLE_CACHE_ALIGNED;
};
typedef struct _node_t node_t;

// Support 127 threads.
#define HANDLES	128
struct _obqueue_t {
	struct _node_t* init_node;
	volatile long init_id DOUBLE_CACHE_ALIGNED;
	
	volatile long put_index DOUBLE_CACHE_ALIGNED;
	volatile long pop_index DOUBLE_CACHE_ALIGNED;

	struct _handle_t* volatile enq_handles[HANDLES];
	struct _handle_t* volatile deq_handles[HANDLES];
	
	int threshold;

	pthread_barrier_t enq_barrier;
	pthread_barrier_t deq_barrier;
};
typedef struct _obqueue_t obqueue_t;

struct _handle_t {
	struct _handle_t* next DOUBLE_CACHE_ALIGNED;
  struct _node_t * spare;
	
	struct _node_t* volatile put_node CACHE_ALIGNED;
	struct _node_t* volatile pop_node CACHE_ALIGNED;
};
typedef struct _handle_t handle_t;

static inline node_t* ob_new_node() {
    node_t* n = align_malloc(PAGE_SIZE, sizeof(node_t));
    memset(n, 0, sizeof(node_t));
    return n;
}

#define ENQ	(1 << 1)
#define DEQ	(1 << 0)

// regiseter the enqueuers first, dequeuers second.
void ob_queue_register(obqueue_t* q, handle_t* th, int flag) {
	th->next = NULL;
	th->spare = ob_new_node();
	th->put_node = th->pop_node = q->init_node;
	
    if(flag & ENQ) {
		handle_t** tail = q->enq_handles;
		for(int i = 0; ; ++i) {
			handle_t* init = NULL;
			if(tail[i] == NULL && CAS(tail + i, &init, th)) {
				break;
			}
		}
		// wait for the other enqueuers to register.
		pthread_barrier_wait(&q->enq_barrier);
  	}
	
	if(flag & DEQ) {
		handle_t** tail = q->deq_handles;
		for(int i = 0; ; ++i) {
			handle_t* init = NULL;	
			if(tail[i] == NULL && CAS(tail + i, &init, th)) {
				break;
			}	
		}
		// wait for the other dequeuers to register.
		pthread_barrier_wait(&q->deq_barrier);
	}
}

void ob_init_queue(obqueue_t* q, int enqs, int deqs, int threshold) {
	q->init_node = ob_new_node();
	q->threshold = threshold;
	q->put_index = q->pop_index = q->init_id = 0;

	// We take enqs enqueuers, deqs dequeuers. 
	pthread_barrier_init(&q->enq_barrier, NULL, enqs);
	pthread_barrier_init(&q->deq_barrier, NULL, deqs);
}

/*
 * ob_find_cell: This is our core operation, locating the offset on the nodes and nodes needed.
 */
static void *ob_find_cell(node_t* volatile* ptr, long i, handle_t* th) {
	// get current node
    node_t *curr = *ptr;
	/*j is thread's local node'id(put node or pop node), (i / N) is the cell needed node'id.
	  and we shoud take it, By filling the nodes between the j and (i / N) through 'next' field*/ 
    for (long j = curr->id; j < i / N; ++j) {
        node_t *next = curr->next;
		// next is NULL, so we Start filling.
        if (next == NULL) {
			// use thread's standby node.
            node_t *temp = th->spare;
            if (!temp) {
                temp = ob_new_node();
                th->spare = temp;
            }
			// next node's id is j + 1.
            temp->id = j + 1;
			// if true, then use this thread's node, else then thread has have done this.
            if (CASra(&curr->next, &next, temp)) {
                next = temp;
				// now thread there is no standby node.
                th->spare = NULL;
            }
        }
		// take the next node.
        curr = next;
    }
	// update our node to the present node.
    *ptr = curr;
	// Orders processor execution, so other thread can see the '*ptr = curr'.
	asm ("sfence" ::: "cc", "memory");
	// now we get the needed cell, its' node is curr and index is i % N.
    return &curr->cells[i % N];
}

void ob_enqueue(obqueue_t *q, handle_t *th, void *v) {
	for(;;) {
		// FAAcs(&q->put_index, 1) return the needed index.
		void* volatile* c = ob_find_cell(&th->put_node, FAAcs(&q->put_index, 1), th);
		// now c is the nedded cell
		void* cv;
		/* if XCHG(ATOMIC: XCHG—Exchange Register/Memory with Register) 
			return BOT, so our value has put into the cell, just return.*/
		if((cv = XCHG(c, v)) == BOT)
			return;
		/* else the couterpart pop thread has wastage this cell, so we just try again*/
	}
}

void *ob_dequeue(obqueue_t *q, handle_t *th) {
	handle_t* init_th = th;
	int times;
	void* cv = BOT;
	long index;
	for(;;) {	
		// index is the needed pop_index.
		index = FAAcs(&q->pop_index, 1);
		// locate the needed cell.
		void* volatile* c = ob_find_cell(&th->pop_node, index, th);
		// because the queue is a non-blocking queue, so we just use 1024 spins.
		times = (1 << 10);
		do {
			cv = *c;
			if(cv)
				goto over;
			__asm__ ("pause");
		} while(times-- > 0);
		// XCHG, if return BOT so this cell is NULL, we wastage this cell~~
        if((cv = XCHG(c, TOP)) == BOT) {
			// if our index is greater than put_index, we just return NULL, because we've seen empty queues
			long put_index;
            if(index >= (put_index = q->put_index)) {
                while(!CAS(&q->put_index, &put_index, index + 1) && put_index <= index);
                break;
            }
			// else there is more datas, so we try again.
            continue;
        }
        break;
	}	
	over:
	/* if the index is the node's last cell: (NBITS == 4095), it Try to reclaim the memory.
	 * so we just take the smallest ID node that is not reclaimed(init_node), and At the same time, by traversing     
	 * the local data of other threads, we get a larger ID node(min_node). 
	 * So it is safe to recycle the memory [init_node, min_node).
	 */
	if((index & NBITS) == NBITS) {
		long init_index = ACQUIRE(&q->init_id);
		if((th->pop_node->id - init_index) >= q->threshold 
			&& init_index >= 0
			&& CASa(&q->init_id, &init_index, -1)) {
			
			node_t* init_node = q->init_node;
            th = q->deq_handles[0]; 
			node_t* min_node = th->pop_node;

			int i;	
            handle_t* next = q->deq_handles[i = 1];
            while(next != NULL) {
                node_t* next_min = next->pop_node;
                if(next_min->id < min_node->id)
                    min_node = next_min;
				if(min_node->id <= init_index)
					break;
                next = q->deq_handles[++i];
            }
			
           	next = q->enq_handles[i = 0]; 
			while(next != NULL) {
                node_t* next_min = next->put_node;
                if(next_min->id < min_node->id)
                    min_node = next_min;
                if(min_node->id <= init_index)
                    break;
                next = q->enq_handles[++i];
            }
			
			long new_id = min_node->id;
			if(new_id <= init_index)
				RELEASE(&q->init_id, init_index);
			else {
				q->init_node = min_node;
				RELEASE(&q->init_id, new_id);

				do {
					node_t* tmp = init_node->next;
					free(init_node);
					init_node = tmp;
				} while(init_node != min_node);
			}	
		}
	}
	return cv;
}
