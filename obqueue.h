#include "align.h"
#include "primitives.h"
#define NODE_SIZE (1 << 12)
#define N	NODE_SIZE
#define NBITS	(N - 1)
#define BOT ((void *)0)
#define TOP ((void *)-1)
#include <linux/futex.h>
#include <syscall.h>

struct _node_t {
	struct _node_t* volatile next DOUBLE_CACHE_ALIGNED;
	long id DOUBLE_CACHE_ALIGNED;
	void* volatile cells[NODE_SIZE] DOUBLE_CACHE_ALIGNED;
};
typedef struct _node_t node_t;

#define HANDLES	128
struct _obqueue_t {
	struct _node_t* init_node;
	volatile long init_flag DOUBLE_CACHE_ALIGNED;
	
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
  struct _node_t * spare CACHE_ALIGNED;
	
	struct _node_t* volatile put_node;
	struct _node_t* volatile pop_node;
};
typedef struct _handle_t handle_t;

static inline node_t* new_node() {
    node_t* n = align_malloc(PAGE_SIZE, sizeof(node_t));
    memset(n, 0, sizeof(node_t));
    return n;
}

#define ENQ	2
#define DEQ	1

void queue_register(obqueue_t* q, handle_t* th, int flag) {
	th->next = NULL;
	th->spare = new_node();
	th->put_node = th->pop_node = q->init_node;
	
    if(flag & ENQ) {
		handle_t** tail = q->enq_handles;
		for(int i = 0; ; ++i) {
			handle_t* init = NULL;
			if(tail[i] == NULL && CAS(tail + i, &init, th)) {
				break;
			}
		}
  }
	
	pthread_barrier_wait(&q->enq_barrier);
	
	if(flag & DEQ) {
		handle_t** tail = q->deq_handles;
		for(int i = 0; ; ++i) {
			handle_t* init = NULL;	
			if(tail[i] == NULL && CAS(tail + i, &init, th)) {
				break;
			}	
		}	
	}

	pthread_barrier_wait(&q->deq_barrier);
}

void init_queue(obqueue_t* q, int enqs, int deqs, int threshold) {
	q->init_node = new_node();
	q->threshold = threshold;
	q->put_index = q->pop_index = q->init_flag = 0;

	pthread_barrier_init(&q->enq_barrier, NULL, enqs);
	pthread_barrier_init(&q->deq_barrier, NULL, deqs);
}

static void *find_cell(node_t* volatile* ptr, long i, handle_t* th) {
    node_t *curr = *ptr;

    long j;
    for (j = curr->id; j < i / N; ++j) {
        node_t *next = curr->next;

        if (next == NULL) {
            node_t *temp = th->spare;

            if (!temp) {
                temp = new_node();
                th->spare = temp;
            }

            temp->id = j + 1;

            if (CASra(&curr->next, &next, temp)) {
                next = temp;
                th->spare = NULL;
            }
        }

        curr = next;
    }

    *ptr = curr;
    return &curr->cells[i % N];
}

int futex_wake(void* addr, int val){  
  return syscall(SYS_futex, addr, FUTEX_WAKE, val, NULL, NULL, 0);  
}  

void enqueue(obqueue_t *q, handle_t *th, void *v) {
	void* volatile* c = find_cell(&th->put_node, FAAcs(&q->put_index, 1), th);
	void* cv;	
	if((cv = XCHG(c, v)) == BOT)
		return;
	*((int*) cv) = 0;
	futex_wake(cv, 1);	
}

int futex_wait(void* addr, int val){  
    return syscall(SYS_futex, addr, FUTEX_WAIT, val, NULL, NULL, 0);  
}  

void *dequeue(obqueue_t *q, handle_t *th) {
	handle_t* init_th = th;
	int times;
	void* cv;
	int futex_addr = 1;	
	long index = FAAcs(&q->pop_index, 1);
	void* volatile* c = find_cell(&th->pop_node, index, th);
		
		
	times = (1 << 20);
	do {
		cv = *c;
		if(cv)
			goto over;
		__asm__ ("pause");
	} while(times-- > 0);

	if((cv = XCHG(c, &futex_addr)) == BOT) {
		while(futex_addr == 1)
			futex_wait(&futex_addr, 1);
		cv = *c;
	}
	over:
	if((index & NBITS) == NBITS) {
		FENCE();
		long init_index = q->init_flag;
		if(th->pop_node->id - init_index >= q->threshold 
			&& init_index >= 0
			&& CASa(&q->init_flag, &init_index, -1)) {
			
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

			if(min_node->id <= init_index)
				q->init_flag = init_index;
			else {
				q->init_node = min_node;
				q->init_flag = min_node->id;

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
