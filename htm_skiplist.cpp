#include <mutex>
#include <thread>
#include <iostream>
#include <chrono>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>
#include <immintrin.h>

using namespace std;
using namespace chrono;

#define FORCED_ABORT -10

static const int NUM_TEST = 4000000;
static const int RANGE = 1000;
static const int MAX_LEVEL = 10;

atomic<int> g_num_tx_aborts{ 0 };
atomic<int> g_num_tx_abort_capacity{ 0 };
atomic<int> g_num_tx_abort_confilct{ 0 };
atomic<int> g_num_tx_abort_forced{ 0 };
atomic<int> g_num_tx_abort_explicit{ 0 };
atomic<int> g_num_tx_abort_rest{ 0 };

atomic<int> g_num_tx_commits{ 0 };


thread_local unsigned int num_tx_aborts = 0;
thread_local unsigned int num_tx_abort_capacity = 0;
thread_local unsigned int num_tx_abort_confilct = 0;
thread_local unsigned int num_tx_abort_forced = 0;
thread_local unsigned int num_tx_abort_explicit = 0;
thread_local unsigned int num_tx_abort_rest = 0;

thread_local unsigned int num_tx_commits = 0;

unsigned long fast_rand(void)
{ //period 2^96-1
    static thread_local unsigned long x = 123456789, y = 362436069, z = 521288629;
    unsigned long t;
    x ^= x << 16;
    x ^= x >> 5;
    x ^= x << 1;

    t = x;
    x = y;
    y = z;
    z = t ^ x ^ y;

    return z;
}

int tx_start()
{
	int status = 0;
	
    status = _xbegin();
    if (_XBEGIN_STARTED == (unsigned)status) {
        return status;
    }
    ++num_tx_aborts;
    if (status & _XABORT_CAPACITY) {
        ++num_tx_abort_capacity;
	} else if (status & _XABORT_CONFLICT) {
        ++num_tx_abort_confilct;
    } else if (_XABORT_CODE(status) == 0xaa) {
        ++num_tx_abort_forced;
        return FORCED_ABORT;
    } else if (status & _XABORT_EXPLICIT) {
        ++num_tx_abort_explicit;
	} else {
        ++num_tx_abort_rest;
	}

    return status;
}

int tx_end()
{
    _xend();
    ++num_tx_commits;
    return 0;
}


class SKNode
{
public:
    int key;
    SKNode* next[MAX_LEVEL];
    int topLevel;
    volatile bool deleted{ false };

    SKNode() {
        for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = nullptr;
		}
		topLevel = MAX_LEVEL;
    }

    SKNode(int key) : key(key) {
        for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = nullptr;
		}
		topLevel = MAX_LEVEL;
    }

    SKNode(int key, int height) : key(key), topLevel(height) {
        for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = nullptr;
		}
    }

    void InitNode() {
        key = 0;
        topLevel = MAX_LEVEL;
        for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = nullptr;
		}
    }

    void InitNode(int x, int top) {
        key = x;
        topLevel = top;
        for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = nullptr;
		}
    }
};

class HTMSKSET
{
    SKNode* head;
    SKNode* tail;
    
    mutex glock;
public:
    HTMSKSET() {
        head = new SKNode(0x80000000);
        tail = new SKNode(0x7FFFFFFF);
        for (int i = 0; i < MAX_LEVEL; i++) {
			head->next[i] = tail;
		}
    }

    void Init()
    {
        SKNode* curr = head->next[0];
        while (curr != tail) {
			SKNode* temp = curr;
			curr = curr->next[0];
			delete temp;
		}
		for (int i = 0; i < MAX_LEVEL; i++) {
			head->next[i] = tail;
		}
    }

    void Dump()
	{
		SKNode* curr = head;
		printf("First 20 entries are : ");
		for (int i = 0; i < 20; ++i) {
			curr = curr->next[0];
			if (NULL == curr) break;
			printf("%d(%d), ", curr->key, curr->topLevel);
		}
		printf("\n");
	}

    bool SeqAdd(SKNode* newNode)
    {
        lock_guard<mutex> lg(glock);
        
        SKNode* preds[MAX_LEVEL];
        SKNode* succs[MAX_LEVEL];
        Find(newNode->key, preds, succs);
        
        if(succs[0]->key == newNode->key) return false;

        for(int l = 0; l <= newNode->topLevel; ++l) {
            newNode->next[l] = succs[l];
            preds[l]->next[l] = newNode;
        }

        atomic_thread_fence(memory_order_release);

        return true;
    }

    void Find(int key, SKNode* preds[MAX_LEVEL], SKNode* currs[MAX_LEVEL])
	{
		int cl = MAX_LEVEL - 1;
		while (true) {
			if (MAX_LEVEL - 1 == cl)
				preds[cl] = head;
			else preds[cl] = preds[cl + 1];
			currs[cl] = preds[cl]->next[cl];
			while (currs[cl]->key < key) {
				preds[cl] = currs[cl];
				currs[cl] = currs[cl]->next[cl];
			}
			if (0 == cl) return;
			cl--;
		}
	}

    bool Add(int x)
    {
        int topLevel = 0;
        while((fast_rand() % 2) == 1)
        {
            ++topLevel;
            if(topLevel >= MAX_LEVEL - 1) break;

        }
        SKNode* newNode = new SKNode;
        newNode->InitNode(x, topLevel);

        SKNode* preds[MAX_LEVEL];
        SKNode* succs[MAX_LEVEL];

        int retry = -1;
        
    start_from_scratch_i:
        ++retry;
        //if(retry >= MAX_RETRIES) {
        //    return SeqAdd(newNode);
        //}
        Find(x, preds, succs);
        if(x == succs[0]->key) {
            atomic_thread_fence(memory_order_acquire);
            if (succs[0]->deleted) goto start_from_scratch_i;
            delete newNode;
            return false;
        } 
        else{
            atomic_thread_fence(memory_order_acquire);
            int status = tx_start();
            if (status != _XBEGIN_STARTED){
                goto start_from_scratch_i;
            }
            // check consistency
            for(int l = 0; l <= topLevel; ++l) {
                if(preds[l]->next[l] != succs[l] || preds[l]->deleted || succs[l]->deleted ){
                    if (_xtest())
                        _xabort(0xaa);
                    else {
                        ++num_tx_abort_forced;
                        goto start_from_scratch_i;
                    }
                }
            }

            for(int l = 0; l <= topLevel; ++l){
                newNode->next[l] = succs[l];
            }
            for(int l = 0; l <= topLevel; ++l){
                preds[l]->next[l] = newNode;
            }
            tx_end();
            atomic_thread_fence(memory_order_release);
            return true;
        }
    }

    bool Remove(int x)
    {
        SKNode* preds[MAX_LEVEL];
        SKNode* succs[MAX_LEVEL];

        int retry = -1;
    start_from_scratch_d:
        ++retry;
        Find(x, preds, succs);

        atomic_thread_fence(memory_order_acquire);
        int status = tx_start();
        if (status != _XBEGIN_STARTED){
            goto start_from_scratch_d;
        }
        else
        {
            if(succs[0]->key == x)
            {
                for(int l = 0; l <= succs[0]->topLevel ; ++l)
                {
                    if(preds[l]->next[l] != succs[0] || preds[l]->deleted) {
                        if (_xtest())
                            _xabort(0xaa);
                        else {
                            ++num_tx_abort_forced;
                            goto start_from_scratch_d;
                        }
                    }

                }

                succs[0]->deleted = true;
                for(int l = 0; l < succs[0]->topLevel; ++l)
                {
                    preds[l]->next[l] = succs[0]->next[l];
                }
                tx_end();
                atomic_thread_fence(memory_order_release);
                //delete succs[0]; delete X
                return true;
            }
            else{
                tx_end();
                return false;
            }
            
        }
    }

    bool Contains(int x)
    {
        SKNode* pred = head;
		SKNode* curr = NULL;
        for (int level = MAX_LEVEL - 1; level >= 0; --level){
            curr = pred->next[level];
            while (true)
            {
                while(curr->deleted)
                {
                    curr = curr->next[level];
                }

                if (curr->key < x) {
				    pred = curr;
				    curr = pred->next[level];
			    }
                else{
                    break;
                }
            }
        }
        return (curr->key == x && curr->deleted == false);
    }
};


HTMSKSET my_set;

void benchmark(int num_thread)
{
	for (int i = 0; i < NUM_TEST / num_thread; ++i) {
		//	if (0 == i % 100000) cout << ".";
		switch (fast_rand() % 3) {
		case 0: my_set.Add(fast_rand() % RANGE); break;
		case 1: my_set.Remove(fast_rand() % RANGE); break;
		case 2: my_set.Contains(fast_rand() % RANGE); break;
		default: cout << "ERROR!!!\n"; exit(-1);
		}
	}

    g_num_tx_aborts.fetch_add(num_tx_aborts);
    g_num_tx_abort_capacity.fetch_add(num_tx_abort_capacity);
    g_num_tx_abort_confilct.fetch_add(num_tx_abort_confilct);
    g_num_tx_abort_forced.fetch_add(num_tx_abort_forced);
    g_num_tx_abort_explicit.fetch_add(num_tx_abort_explicit);
    g_num_tx_abort_rest.fetch_add(num_tx_abort_rest);
    g_num_tx_commits.fetch_add(num_tx_commits);
}

int main()
{
	vector <thread> worker;
	for (int num_thread = 1; num_thread <= 16; num_thread *= 2) {
		my_set.Init();
		worker.clear();

        g_num_tx_aborts.store(0);
        g_num_tx_abort_capacity.store(0);
        g_num_tx_abort_confilct.store(0);
        g_num_tx_abort_forced.store(0);
        g_num_tx_abort_explicit.store(0);
        g_num_tx_abort_rest.store(0);

        g_num_tx_commits.store(0);


		auto start_t = high_resolution_clock::now();
		for (int i = 0; i < num_thread; ++i)
			worker.push_back(thread{ benchmark, num_thread });
		for (auto& th : worker) th.join();
		auto du = high_resolution_clock::now() - start_t;
		my_set.Dump();

        cout << "total aborts: " << g_num_tx_aborts << endl;
        cout << "capacity aborts: " << g_num_tx_abort_capacity << endl;
        cout << "conflict aborts: " << g_num_tx_abort_confilct << endl;
        cout << "forced aborts: " << g_num_tx_abort_forced << endl;
        cout << "explicit aborts: " << g_num_tx_abort_explicit << endl;
        cout << "rest aborts: " << g_num_tx_abort_rest << endl;
        cout << "total commits: " << g_num_tx_commits << endl;

		cout << num_thread << " Threads,  Time = ";
		cout << duration_cast<milliseconds>(du).count() << " ms\n";
	}
}
