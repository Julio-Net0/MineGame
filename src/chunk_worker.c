#include "chunk_worker.h"
#include <pthread.h>
#include <raylib.h>

//Needs to be bigger than MAX_ACTIVE_CHUNKS
#define QUEUE_SIZE 2048

static Chunk* generationQueue[QUEUE_SIZE];
static int queueHead = 0;
static int queueTail = 0;

static pthread_t workerThread;
static pthread_mutex_t queueMutex;
static bool workerRunning = false;

static void* WorkerLoop(void* arg) {
  TraceLog(LOG_INFO, "WORKER THREAD ID: %p", (void*)pthread_self());
  while (workerRunning) {
    Chunk *target = NULL;

    pthread_mutex_lock(&queueMutex);
    if (queueHead != queueTail) {
      target = generationQueue[queueHead];
      queueHead = (queueHead + 1) % QUEUE_SIZE;
    }
    pthread_mutex_unlock(&queueMutex);

    if (target != NULL) {
      GenerateChunkTerrain(target);

      target->isGenerating = false;
      target->terrainJustGenerated = true;
      target->isGenerated = true; 

    } else {
      WaitTime(0.001); 
    }
  }
  return NULL;
}

void InitChunkWorker(void) {
    workerRunning = true;
    pthread_mutex_init(&queueMutex, NULL);
    pthread_create(&workerThread, NULL, WorkerLoop, NULL);
}

void CloseChunkWorker(void) {
    workerRunning = false;
    pthread_join(workerThread, NULL);
    pthread_mutex_destroy(&queueMutex);
}

void EnqueueChunkGeneration(Chunk *chunk) {
    pthread_mutex_unlock(&queueMutex);

    int nextTail = (queueTail + 1) % QUEUE_SIZE;

    if (nextTail != queueHead) {
      chunk->isGenerating = true;
      chunk->isGenerating = false;

      generationQueue[queueTail] = chunk;
      queueTail = nextTail;
    }

    pthread_mutex_unlock(&queueMutex);
}
