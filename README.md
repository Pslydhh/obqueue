# obqueue
obqueue.h is a awesome fast/simple concurrent queue, and the dequeue is blocking while there is no elements.
obqueue_no_blocking.h is non-blocking relative to obqueue.h, dequeue just return NULL while there is no elements.

# Prohibition
0(NULL) unable to enter the queue.
# Notes
1: 
The long type is -2^63 ~ (2^63-1), Even call 100 million times per second(so Absolutely not so much in the actual environment), We can continue to use it for  (2^63-1) / 100,000,000 / 3600(seconds) / 24(hours) / 365(days) == 2924.71209 (years)！

2: 
if there is one thread(It is likely to be dequeuer) Particularly slow, May slow down memory reclaim, so every consumer should be consumed on average.
# test_obqueue_ 
gcc -pthread -g -o test_obq test_obq.c

./test_obq 2500000 8

# use case(obqueue.h)：test_obq.c
<pre><code>
#include "obqueue.h"

#define THREAD_NUM 4
long COUNTS_PER_THREAD = 2500000;
int threshold = 8;
obqueue_t qq;

void* producer(void* index) {
	obqueue_t* q = &qq;		
	handle_t* th = (handle_t*) malloc(sizeof(handle_t));
	memset(th, 0, sizeof(handle_t));
	// register as enqueuer.
	ob_queue_register(q, th, ENQ);
	
	for(int i = 0; i < COUNTS_PER_THREAD; ++i)  
		ob_enqueue(q, th, 1 + i + ((int) index) * COUNTS_PER_THREAD);
	return NULL;
}

int* array;
void* consumer(void* index) {

	obqueue_t* q = &qq;
	handle_t* th = (handle_t*) malloc(sizeof(handle_t));
	memset(th, 0, sizeof(handle_t));
	// register as dequeuer.
	ob_queue_register(q, th, DEQ);
	
	for(long i = 0; i < COUNTS_PER_THREAD; ++i)  {
		int value;
		if((value = ob_dequeue(q, th)) == NULL)
			return NULL;
		array[value] = 1;		 
	}
	return NULL;
}

int main(int argc, char* argv[]) {

	printf("\nthread number: %d\n", THREAD_NUM);
	
	printf("cpu: %d\n", get_nprocs_conf());
	if(argc >= 3) {
		COUNTS_PER_THREAD = atol(argv[1]);
		threshold = atoi(argv[2]);	
	}
	
	printf("take %ld ops\n", THREAD_NUM * COUNTS_PER_THREAD);
	fflush(stdout);

	array = (int*) malloc((1 + THREAD_NUM * COUNTS_PER_THREAD) * sizeof(int));
	memset(array, 0, (1 + THREAD_NUM * COUNTS_PER_THREAD) * sizeof(int));
	ob_init_queue(&qq, THREAD_NUM, THREAD_NUM, threshold);

	struct timeval start;
	gettimeofday(&start, NULL);
	
	pthread_t pids[THREAD_NUM];
	
	for(int i = 0; i < THREAD_NUM; ++i) {
		if(-1 == pthread_create(&pids[i], NULL, producer, i)) {
			printf("error create thread\n");
			exit(1);
		}
	}

	for(int i = 0; i < THREAD_NUM; ++i) {
		if(-1 == pthread_create(&pids[i], NULL, consumer, i)) {
			printf("error create thread\n");
			exit(1);
		}
	}

	int error_code;
	for(int i = 0; i < THREAD_NUM; ++i) {
		if(error_code = pthread_join(pids[i], NULL)) {
			printf("error_code is %d\n", error_code);
			exit(1);
		}
	}

	struct timeval pro_end;
	gettimeofday(&pro_end, NULL);

	int verify = 1;
	for(int j = 1; j <= THREAD_NUM * COUNTS_PER_THREAD; ++j) {
		if(array[j] != 1) {
			printf("Error: ints[%d]\n", j);
			verify = 0;
			break;
		}
	}
	if(verify)
		printf("ints[1-%ld] has been Verify through\n", THREAD_NUM * COUNTS_PER_THREAD);

	float cost_time = (pro_end.tv_sec-start.tv_sec)+(pro_end.tv_usec-start.tv_usec) / 1000000.0;
	printf("cost times: %f seconds\n\n", cost_time);
	fflush(stdout);
	return 0;
}


</pre></code>
# use case(obqueue_no_blocking.h)：test_obq_no_blocking.c
<pre><code>
#include "obqueue_no_blocking.h"

long COUNTS_PER_THREAD = 2500000;
int threshold = 8;
obqueue_t qq;

int* array;
void* produce_and_consume(void* index) {
	obqueue_t* q = &qq;		
	handle_t* th = (handle_t*) malloc(sizeof(handle_t));
	memset(th, 0, sizeof(handle_t));
	ob_queue_register(q, th, ENQ | DEQ);
	
	for(int i = 0; i < COUNTS_PER_THREAD; ++i) {
		ob_enqueue(q, th, 1 + i + ((int) index) * COUNTS_PER_THREAD);
		int value;
		if((value = ob_dequeue(q, th)) == NULL)
			return NULL;
		array[value] = 1;
	}
	return NULL;
}

#define THREAD_NUM 4

int main(int argc, char* argv[]) {

	printf("thread number: %d\n", THREAD_NUM);
	printf("cpu: %d\n", get_nprocs_conf());
	if(argc >= 3) {
		COUNTS_PER_THREAD = atol(argv[1]);
		threshold = atoi(argv[2]);	
	}
	
	printf("here %ld\n", THREAD_NUM * COUNTS_PER_THREAD);
	array = (int*) malloc((1 + THREAD_NUM * COUNTS_PER_THREAD) * sizeof(int));
	memset(array, 0, (1 + THREAD_NUM * COUNTS_PER_THREAD) * sizeof(int));
	fflush(stdout);
	ob_init_queue(&qq, THREAD_NUM, THREAD_NUM, threshold);

	struct timeval start;
	gettimeofday(&start, NULL);
	
	pthread_t pids[THREAD_NUM];
	for(int i = 0; i < THREAD_NUM; ++i) {
		if(-1 == pthread_create(&pids[i], NULL, produce_and_consume, i)) {
			printf("error create thread\n");
			exit(1);
		}
	}
	
	int error_code;
	for(int i = 0; i < THREAD_NUM; ++i) {
		if(error_code = pthread_join(pids[i], NULL)) {
			printf("error_code is %d\n", error_code);
			exit(1);
		}
	}
	
	struct timeval pro_end;
	gettimeofday(&pro_end, NULL);

	int verify = 1;
	for(int j = 1; j <= THREAD_NUM * COUNTS_PER_THREAD; ++j) {
		if(array[j] != 1) {
			printf("Error: ints[%d]\n", j);
			verify = 0;
			break;
		}
	}
	if(verify)
		printf("ints[1-%ld] has been Verify through\n", THREAD_NUM * COUNTS_PER_THREAD);
		
	float cost_time = (pro_end.tv_sec-start.tv_sec)+(pro_end.tv_usec-start.tv_usec) / 1000000.0;
	printf("pro&con cost times: %f seconds\n", cost_time);
	fflush(stdout);
	return 0;
}
</pre></code>
