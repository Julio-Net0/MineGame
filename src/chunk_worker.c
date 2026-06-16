#include "chunk_worker.h"
#include "world_save.h"
#include <pthread.h>
#include <raylib.h>
#include <stdatomic.h>

//Needs to be bigger than MAX_ACTIVE_CHUNKS
#define QUEUE_SIZE 2048

static Chunk* generationQueue[QUEUE_SIZE];
static int queueHead = 0;
static int queueTail = 0;

#define WORKER_THREAD_COUNT 4

static pthread_t workerThreads[WORKER_THREAD_COUNT];
static pthread_mutex_t queueMutex;
static pthread_cond_t workAvail;
static atomic_bool workerRunning = false;

static void* WorkerLoop(void* arg) {
  (void)arg;
  while (1) {
    pthread_mutex_lock(&queueMutex);

    while (workerRunning && queueHead == queueTail) {
      pthread_cond_wait(&workAvail, &queueMutex);
    }

    if (!workerRunning && queueHead == queueTail) {
      pthread_mutex_unlock(&queueMutex);
      break;
    }

    Chunk *target = generationQueue[queueHead];
    queueHead = (queueHead + 1) % QUEUE_SIZE;

    pthread_mutex_unlock(&queueMutex);

    if (!LoadChunkFromDisk(target)) {
      GenerateChunkTerrain(target);
    }

    target->terrainJustGenerated = true;
    target->isGenerated = true;
    target->isGenerating = false;
  }
  return NULL;
}

void InitChunkWorker(void) {
    workerRunning = true;
    pthread_mutex_init(&queueMutex, NULL);
    pthread_cond_init(&workAvail, NULL);
    for (int i = 0; i < WORKER_THREAD_COUNT; i++) {
        pthread_create(&workerThreads[i], NULL, WorkerLoop, NULL);
    }
}

void CloseChunkWorker(void) {
    workerRunning = false;
    pthread_cond_broadcast(&workAvail);
    for (int i = 0; i < WORKER_THREAD_COUNT; i++) {
        pthread_join(workerThreads[i], NULL);
    }
    pthread_cond_destroy(&workAvail);
    pthread_mutex_destroy(&queueMutex);
}

void EnqueueChunkGeneration(Chunk *chunk) {
    pthread_mutex_lock(&queueMutex);

    int nextTail = (queueTail + 1) % QUEUE_SIZE;

    if (nextTail != queueHead) {
      chunk->isGenerating = true;
      chunk->isGenerated = false;

      generationQueue[queueTail] = chunk;
      queueTail = nextTail;
      pthread_cond_signal(&workAvail);
    } else {
      TraceLog(LOG_WARNING, "Chunk generation queue full! Chunk rejected at: %d, %d, %d", chunk->chunkX, chunk->chunkY, chunk->chunkZ);
      chunk->isGenerating = false;
      chunk->isGenerated = false;
    }

    pthread_mutex_unlock(&queueMutex);
}
