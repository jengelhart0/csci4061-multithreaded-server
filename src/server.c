/* csci4061 S2016 Assignment 4 
* section: 7 
* date: 12/09/2016 
* name: Joey Engelhart
* UMN Internet ID, Student ID: engel429
*/

#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "util.h"
#include <sys/time.h>

#define MAX_THREADS 100
#define MAX_QUEUE_SIZE 100
#define MAX_CACHE_SIZE 100 

#define LOG_WRITE_FLAGS O_WRONLY | O_APPEND | O_CREAT
#define LOG_PERMS 0666

/* arrays for request type determination */
static const char *exts[] = {"html", "htm", "jpg", "gif", "other"};
static char *types[] = {"text/html", "text/html", "image/jpeg", "image/gif", "text/plain"};

/* arrays for canceling threads if necessary */
static pthread_t dispatchers[MAX_THREADS];
static pthread_t workers[MAX_THREADS];

static int total_dispatchers;
static int total_workers;

/* queue structures and data */
typedef struct request_queue_entry
{
        int m_socket;
        char m_szRequest[MAX_REQUEST_LENGTH];
} request_queue_entry_t;

typedef struct request_queue_node
{
	request_queue_entry_t queue_entry;
	struct request_queue_node *next;
} queue_node_t;

static queue_node_t *request_queue;
static queue_node_t *last_in_queue;
static int requests_in_queue = 0;
static int queue_size;

/* cache structures and data */
typedef struct cache_entry
{
	char *filename;
	char *type;
	int bytes;
	char *data;
} cache_entry_t;

static int cache_size;
static cache_entry_t **cache;

/* synchronization structures */
static pthread_cond_t no_requests = PTHREAD_COND_INITIALIZER;
static pthread_cond_t queue_full = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t id_lock = PTHREAD_MUTEX_INITIALIZER;

/* logging structures */
static const char *SERVER_LOG = "web_server_log";
static const char *errorbuf = "File not found";

/* thread for handling interrupts and finished state */
static pthread_t sigthread;

/* miscellaneous data */
static int next_worker_id = 0; 

/****************SHARED HELPER FUNCTIONS*****************/

/******Lock Helpers******/

static void safe_lock(pthread_mutex_t *lock) {
	int error;
	if(error = pthread_mutex_lock(lock)) {
		fprintf(stderr, "Attempt to acquire lock produced an error: %d. Fatal. Exiting\n", error);	
		if(error = pthread_kill(sigthread, SIGUSR1)) {
			fprintf(stderr, "Attempt wake signal thread failed: %d. Fatal: exiting\n", error);	
			exit(-1);
		}	
		pthread_exit(NULL);
	}
}
static void safe_unlock(pthread_mutex_t *lock) {
	int error;
	if(error = pthread_mutex_unlock(lock)) {
		fprintf(stderr, "Attempt to release lock produced an error: %d. Fatal. Exiting\n", error);
		if(error = pthread_kill(sigthread, SIGUSR1)) {
			fprintf(stderr, "Attempt wake signal thread failed: %d. Fatal: exiting\n", error);	
			exit(-1);
		}	
		pthread_exit(NULL);
	}
}

/*****Logging Helper*****/

static int log_request(long tid, int total_requests, int socket, char *filename,
		       int bytes, clock_t *start_time, char *hit_or_miss) {

	int fd;
	char timebuf[32];
	char log_buffer[2048];
	clock_t end_time = clock();
	int request_time = (end_time - *start_time);
	snprintf(timebuf, sizeof(timebuf), "%dms", request_time);
	safe_lock(&log_lock);
	if((fd = open(SERVER_LOG, LOG_WRITE_FLAGS, LOG_PERMS)) == -1) {
		perror("Failed to open web server log for write.");
		return -1;	
	}
	if(bytes < 0) {
		snprintf(log_buffer, sizeof(log_buffer), "[%ld][%d][%d][%s][%s][%s][%s]\n", tid, total_requests,
          		 socket, filename, errorbuf, timebuf, hit_or_miss);
	} else {
		snprintf(log_buffer, sizeof(log_buffer), "[%ld][%d][%d][%s][%d][%s][%s]\n", tid, total_requests,
		 	 socket, filename, bytes, timebuf, hit_or_miss); 
	}
	if(write(fd, log_buffer, strlen(log_buffer)) <= 0) {
		perror("Failed to write log to request log.");
		return -1;	
	}	
	if(close(fd) == -1) {
		perror("Failed to close request log.");
		return -1;	
	}
	safe_unlock(&log_lock);
	return 0;
}

/*******THREAD FUNCTIONS AND HELPERS*******/

/***Clean-up thread and resource release helper***/
/* this clean-up thread function handles resource clean up 
 * if no more work can be done or an interrupt arrives.
 * Since the main thread blocks SIGINT before creating all
 * threads, and this one specifically sigwaits for SIGINT,
 * this thread will be the only one that responds to SIGINT.
 * Additionally, this thread is signaled when requests_in_queue
 * and dispatchers_left are both 0.
 */

/**Resources Release**/

void release_resources(void) {
	int m;
	int r;
	queue_node_t *next_to_free;
	queue_node_t *free_now;
	next_to_free = request_queue;
	for(r = 0; r < requests_in_queue; r++) {
		free_now = next_to_free;
		next_to_free = free_now->next;
		free(free_now);
	}
	for(m = 0; m < cache_size; m++) {
		if(*(cache + m) != NULL) {
			free((**(cache + m)).data);
			free((**(cache + m)).filename);
			free(*(cache + m));
		}	
	}	
	free(cache);

	pthread_cond_destroy(&queue_full);
	pthread_cond_destroy(&no_requests);
	pthread_mutex_destroy(&queue_lock);
	pthread_mutex_destroy(&cache_lock);
	pthread_mutex_destroy(&log_lock);
	pthread_mutex_destroy(&id_lock);

}
/**Clean-up thread function**/

static void *term_handler(void *arg) {
	sigset_t termmask;
	int sig;
	if(sigemptyset(&termmask) == -1 ||
 	   sigaddset(&termmask, SIGINT) == -1 ||
 	   sigaddset(&termmask, SIGUSR1) == -1 ||
	   sigwait(&termmask, &sig) == -1) {
		fprintf(stderr, "Signal handling thread errors\n");	
		release_resources();
		exit(-1);
	}
	// cancel any remaining threads, in case SIGINT caused return from sigwait().
	int i;
	for(i = 0; i < total_dispatchers; i++) {
		pthread_cancel(dispatchers[i]);
	}
	for(i = 0; i < total_workers; i++) {
		pthread_cancel(workers[i]);
	}
	release_resources();
	exit(-1);	
}
/***Dispatcher thread and dispatcher helpers***/

/**Dispatcher helpers**/

void add_request_to_queue(int socket, char *filename) {
	// make a new queue entry
	queue_node_t *new_queue_node;
	int error;

	if((new_queue_node = (queue_node_t *) calloc(1, sizeof(queue_node_t))) == NULL) {
		perror("Allocating new entry in queue failed. Fatal. Exiting.");
		if(error = pthread_kill(sigthread, SIGUSR1)) {
			fprintf(stderr, "Attempt wake signal thread failed: %d. Fatal: exiting\n", error);	
			exit(-1);
		}
	}	
	// enter data into new last queue entry
	(new_queue_node->queue_entry).m_socket = socket;
	snprintf((new_queue_node->queue_entry).m_szRequest, strlen(filename) + 1, "%s", filename);
	if(requests_in_queue) {
		last_in_queue->next = new_queue_node;
		last_in_queue = new_queue_node;
	} else {
		request_queue = new_queue_node;
		last_in_queue = request_queue;
	}
	requests_in_queue++;
}

/**Dispatcher thread function**/
void *dispatch(void *arg) {
	int error;
	int socket;
	char filename[MAX_REQUEST_LENGTH];
	// connection monitoring loop

	while(1) {
		socket = accept_connection();	
		if(!get_request(socket, filename)) {
			safe_lock(&queue_lock);
			while(requests_in_queue == queue_size) {
				if(error = pthread_cond_wait(&queue_full, &queue_lock)) {
					fprintf(stderr, "Failed to wait on queue_full condition variable: Continuing\n");			
				}	
			}
			add_request_to_queue(socket, filename);
			if(error = pthread_cond_signal(&no_requests)) {
				fprintf(stderr, "Produced error upon signaling empty queue condition variable:"
						" %d. Continuing\n", error);	
			}
			safe_unlock(&queue_lock);	
		} else {
			fprintf(stderr, "Failed to get request. Continuing\n");
		}
	}
	return NULL;
}

/***Worker thread and worker helpers***/

/**Worker helpers**/

void get_queue_request(int *socket_ptr, char *filename) {
	// if there is only one request, next_in_queue will be null after this,
	// which is fine here
	queue_node_t *next_in_queue = request_queue->next;

	*socket_ptr = (request_queue->queue_entry).m_socket;
	char *request = (request_queue->queue_entry).m_szRequest;
	// open will fail if first char is a path slash
	if(request[0] == '/' || request[0] == '\\') request++;

	snprintf(filename, strlen(request) + 1, "%s", request);

	requests_in_queue--;
	free(request_queue);
	request_queue = next_in_queue;
}
/*
 * Hash function below is the widely used djb2 string hash:
 * Author: Dan Bernstein
 * Citation: http://www.cse.yorku.ca/~oz/hash.html
 */
static int cache_hash(char *filename) {
	unsigned long hash = 5381;
	int c;
	char *marker = filename;
	while(c = *marker++)
		hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
	return (int) (hash % cache_size);
}
/*
 * Stores request in appropriate cache location by assigning cache pointer to new_entry.
 */
static int store_in_cache(cache_entry_t *new_entry, char *filename) {
	safe_lock(&cache_lock);
	int start_loc = cache_hash(filename);
	int loc = start_loc;
	// if no pointer is stored at this loc, that entry is free
	while((*(cache + loc) != NULL)) {
		if(strcmp(filename, (**(cache + loc)).filename)) {
			loc = (loc + 1) % cache_size;
			// if we return to starting loc, file not in cache and cache is full: replace starting location
			if(loc == start_loc) {
				break;
			}
		} else {
			// file was recently put in cache. already here: return
			safe_unlock(&cache_lock);
			return 1;	
		}
	}
	// if we return to start_loc and cache location isn't NULL, we are replacing existing entry.
	if(*(cache + loc) != NULL) {
		// losing reference to old entry: deallocate
		free((**(cache + loc)).data);
		free(*(cache + loc));
	}			
	*(cache + loc) = new_entry;	
	safe_unlock(&cache_lock);
	return 0;
}
/*
 * Checks the cache for requests. If present, return result.
 */
int service_from_cache(int socket, char *filename, int total_requests, clock_t *start_time, int worker_id) {
	safe_lock(&cache_lock);
	int start_loc = cache_hash(filename);
	int loc = start_loc;
	cache_entry_t entry;  
        // because all cache ptrs were NULLed upon allocation, all non-NULL pointers point
        // to cache entries, either by first time assignment or replacement.
	while(*(cache + loc) != NULL) {
		if(strcmp(filename, (**(cache + loc)).filename)) {
			loc = (loc + 1) % cache_size;	
			// if we return to starting loc, file is not in cache
			if(loc == start_loc) {
				safe_unlock(&cache_lock);
				return 0;
			}
		} else {
			entry = **(cache + loc);
			return_result(socket, entry.type, entry.data, entry.bytes); 
			log_request(worker_id, total_requests, socket, filename,
				    entry.bytes, start_time, "HIT");
			safe_unlock(&cache_lock);
			return 1;	
		}
	}
	safe_unlock(&cache_lock);
	return 0;
}
/*
 * Retrieves request from disk and calls store_in_cache.
 */
int service_from_disk(int socket, char *filename, int total_requests, clock_t *start_time, int worker_id) {
	int totalbytes;
	int bytesread;
	int bytes;
	int fd;
	int ext_idx;
	char *last_dot;
	char *dot_runner;
	char errorbuf[2048];
	struct stat request_stat;
	cache_entry_t *new_cache_entry;

	// open requested file
	if((fd = open(filename, O_RDONLY)) == -1) {
		perror("Failed to open file requested. Continuing.");	
		return_error(socket, errorbuf);
		log_request(worker_id, total_requests, socket, filename,
			    -1, start_time, "MISS");
		return -1;
	} 
	//get requested file size
	if(fstat(fd, &request_stat) == -1) {
		perror("Failed to obtain size of requested file. Continuing.");
		return_error(socket, errorbuf);
		log_request(worker_id, total_requests, socket, filename,
			    -1, start_time, "MISS");
		if(close(fd) == -1) {
			perror("Failed to close open file.");
		}
		return -1;
	}
	bytes = request_stat.st_size;

	// allocate new entry for cache
	if(((new_cache_entry = malloc(sizeof(cache_entry_t))) == NULL) ||
	   ((new_cache_entry->data = malloc(bytes)) == NULL)) {
		perror("Failed to allocate space for new cache entry and data");
		return_error(socket, errorbuf);
		log_request(worker_id, total_requests, socket, filename,
			    -1, start_time, "MISS");
	}
	// set bytes 
	new_cache_entry->bytes = bytes;

	// set filename
	new_cache_entry->filename = filename;

	// set type
	dot_runner = filename;
	// loop while valid char * is not null and char pointed to is not null-terminator
	while(dot_runner && *dot_runner) {
		// incrementing dot_runner in each branch ensures increase of exactly 1 per iteration
		if(!(*dot_runner++ == '.')) continue; 
		else last_dot = dot_runner++;		
	}	
	if(!last_dot) {
		fprintf(stderr, "%s not a valid file type. Request failed. Continuing.\n", filename);
		return_error(socket, errorbuf);
		log_request(worker_id, total_requests, socket, filename,
			    -1, start_time, "MISS");
		return -1;
	}
	// advances extension index until one is found that matches the end of filename
	ext_idx = 0;
	while(strcmp(last_dot + 1, exts[ext_idx]) && strcmp(exts[ext_idx], "other")) {
		ext_idx++;
	}
	new_cache_entry->type = types[ext_idx];

	// set file data
	totalbytes = 0;
	while(totalbytes < bytes) {
		if((bytesread = read(fd, (new_cache_entry->data) + totalbytes, bytes - totalbytes)) <= 0) {
			perror("Read attempt in worker thread failed or read 0 bytes. Continuing");
			return_error(socket, errorbuf);
			log_request(worker_id, total_requests, socket, filename,
				    -1, start_time, "MISS");
			if(close(fd) == -1) {
				perror("Failed to close open file.");
			}
			return -1;
		} 
		totalbytes += bytesread;
	}

	// store new cache entry
	store_in_cache(new_cache_entry, filename);

	if(close(fd) == -1) {
		perror("Failed to close open file.");
		return -1;
	}
	safe_lock(&cache_lock);
	if(return_result(socket, new_cache_entry->type, new_cache_entry->data, new_cache_entry->bytes)) {
		fprintf(stderr, "Failed to return result to client. Continuing.\n");	
		safe_unlock(&cache_lock);
		return 0;
	}
	safe_unlock(&cache_lock);
	log_request(worker_id, total_requests, socket, filename,
		    bytes, start_time, "MISS");	
	return 0;
}

/**Worker thread function**/

/*
 *Gets request from queue. Returns request from cache or disk.
 */
void *worker(void *arg) {
	int id;
	int socket;  
	int total_requests = 0;
	int error;
	char *filename;
	clock_t start_time;
	// get sequential worker thread id
	safe_lock(&id_lock);
	id = next_worker_id++;
	safe_unlock(&id_lock);

	while(1) {	
		/**********Locking queue***********/
		safe_lock(&queue_lock);	
		while(requests_in_queue == 0) {
			if(error = pthread_cond_wait(&no_requests, &queue_lock)) {
				fprintf(stderr, "Failed to wait for no_requests condition variable: %d. Fatal. Exiting.\n", error);
			}
		}
		if((filename = (char *) calloc(sizeof(char), MAX_REQUEST_LENGTH)) == NULL) {
			perror("Failed to allocate space for new filename for thread. Fatal. Exiting.");
		}
		get_queue_request(&socket, filename);
		total_requests++;
		if(error = pthread_cond_signal(&queue_full)) {
			fprintf(stderr, "Failed to signal waiting dispatchers with queue_full variable. %d. Fatal. Exiting\n", error);
		}
		/**********Unlocking queue**********/
		safe_unlock(&queue_lock);
	
		start_time = clock();	

		if(!service_from_cache(socket, filename, total_requests, &start_time, id)) {		
			/* since file was not in cache, we'll create a new cache_entry_t,
		 	 * set its data members, get a cache pointer from the cache, and 
			 * set it to our new cache entry
			 */
			service_from_disk(socket, filename, total_requests, &start_time, id);

		}
	}	
        return NULL;
}

/*******VALIDATORS AND ALLOCATORS**********/

int validate_args(char *path, int num_dispatcher, int num_workers, int queue_size, int cache_size) {
	if(chdir(path) == -1) {
		perror("path does not specify a valid location on server");
		return -1;
	}
	if(num_dispatcher > MAX_THREADS || num_dispatcher < 1) {
		fprintf(stderr, "Number of dispatcher threads %d less than 1 or greater than maximum %d: exiting\n", num_dispatcher, MAX_THREADS);
		return -1;
	}
	if(num_workers > MAX_THREADS || num_workers < 1) {
		fprintf(stderr, "Number of worker threads %d less than 1 or greater than maximum %d: exiting\n", num_workers, MAX_THREADS);
		return -1;
	}
	if(queue_size > MAX_QUEUE_SIZE || queue_size < 1) {
		fprintf(stderr, "Queue size %d less than 1 or greater than maximum %d: exiting\n", queue_size, MAX_QUEUE_SIZE);
		return -1;
	}
	if(cache_size > MAX_CACHE_SIZE || cache_size < 1) {
		fprintf(stderr, "Cache_size %d less than 1 or greater than maximum %d: exiting\n", cache_size, MAX_CACHE_SIZE);
		return -1;
	}
	return 0;
}
/*
 * Sets up signal masks for all threads. Creates all threads and data structures for servicing.
 * Signal mask allows all threads but clean-up thread to block important signals.
 */
int create_resources(int num_dispatcher, int num_workers, int queue_size, int cache_size) {
	int error;
	// set up server_mask to be inherited by all threads this process creates
	sigset_t server_mask;
	if(sigemptyset(&server_mask) == -1 ||
	   sigaddset(&server_mask, SIGINT) == -1 ||
	   sigaddset(&server_mask, SIGUSR1) == -1 ||
	   pthread_sigmask(SIG_BLOCK, &server_mask, NULL) == -1) {
		fprintf(stderr, "Failed to set up server mask in create_resources(): exiting\n");
	}
	// no need to allocate queue: dispatchers do so request-by-request: queue backed by
	// linked list

	// allocate request cache. calloc() used to allow NULL pointer checks.
	if((cache = (cache_entry_t **) calloc(cache_size, sizeof(cache_entry_t *))) == NULL) {
		perror("Failed to callocate space for cache: exiting");
		exit(-1);
	}
	// create signal thread, which will sigwait on signals added to server_mask,
	// in order to terminate gracefully.
	if(error = pthread_create(&sigthread, NULL, term_handler, NULL)) {
		fprintf(stderr, "Failed to create signal thread: %d. Exiting.\n", error);
		exit(-1);
	}
	if(error = pthread_detach(sigthread)) {
		fprintf(stderr, "Failed to detach signal thread: %d. Exiting.\n", error);
		exit(-1);
	}

	int i;
	// set total_dispatchers in order to mark the dispatchers[] bound up to which term_handler
	// should check for cancelation eligibility
	total_dispatchers = num_dispatcher;
	// create dispatcher threads
	for(i = 0; i < num_dispatcher; i++) {
		if(pthread_create(dispatchers + i, NULL, dispatch, NULL) == -1) {
			fprintf(stderr, "Failed to create dispatch thread: %d. Exiting.\n", error);
			exit(-1);	
		}
		if(error = pthread_detach(dispatchers[i])) { 
			fprintf(stderr, "Failed to detach dispatch thread: %d. Exiting.\n", error);	
			exit(-1);
		}
	}
	// set total_workers in order to mark the workers[] bound up to which term_handler
	// should check for cancelation eligibility
	total_workers = num_workers;
	// create worker threads
	for(i = 0; i < num_workers; i++) {
		if(pthread_create(workers + i, NULL, worker, NULL) == -1) {
			fprintf(stderr, "Failed to create worker thread: %d. Exiting.\n", error);	
			exit(-1);
		}
		if(pthread_detach(workers[i]) == -1) { 
			fprintf(stderr, "Failed to detach worker thread: %d. Exiting.\n", error);	
			exit(-1);	
		}
	}
	return 0;
}
int main(int argc, char **argv)
{
        //Error check first.
        if(argc != 6 && argc != 7)
        {
                printf("usage: %s port path num_dispatcher num_workers queue_length [cache_size]\n", argv[0]);
                return -1;
        }
	// store args	
	int port = atoi(argv[1]);
	char *path = argv[2];
	int num_dispatcher = atoi(argv[3]);
	int num_workers = atoi(argv[4]);
	queue_size =  atoi(argv[5]);
	cache_size = atoi(argv[6]);

	if(validate_args(path, num_dispatcher, num_workers, queue_size, cache_size) == -1) {
		return -1;
	}
	
        // Call init() first and make dispatcher and worker threads
	// errors within init() & create_resources() automatically terminate process
	init(port);
	create_resources(num_dispatcher, num_workers, queue_size, cache_size);
        pthread_exit(NULL);
}
