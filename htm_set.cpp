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

class NODE {
public:
	int key;
	NODE* next;
	bool removed;

	NODE() {
		next = nullptr;
		removed = false;
	}
	NODE(int x) {
		key = x;
		next = nullptr;
		removed = false;
	}
	~NODE() {
	}
};

class ZSET
{
	NODE head, tail;
public:
	ZSET()
	{
		head.key = 0x80000000;
		tail.key = 0x7FFFFFFF;
		head.next = &tail;
	}
	void Init()
	{
		while (head.next != &tail) {
			NODE* temp = head.next;
			head.next = temp->next;
			delete temp;
		}
	}
	void Dump()
	{
		NODE* ptr = head.next;
		cout << "Result Contains : ";
		for (int i = 0; i < 20; ++i) {
			cout << ptr->key << ", ";
			if (&tail == ptr) break;
			ptr = ptr->next;
		}
		cout << endl;
	}
	bool Validate(NODE* pred, NODE* curr)
	{
		return (false == pred->removed) && (false == curr->removed) && (pred->next == curr);
	}
	bool Add(int x)
	{
		NODE* pred, * curr;

        NODE* newNode = new NODE(x);
        int retry = -1;
    start_from_scratch_i:
        ++retry;

        pred = &head;
        curr = pred->next;
        while (curr->key < x) {
			pred = curr; curr = curr->next;
		}
        atomic_thread_fence(memory_order_acquire);
        int status = tx_start();
        if (status != _XBEGIN_STARTED){
            goto start_from_scratch_i;
        }
        if(!((false == pred->removed) && (false == curr->removed) && (pred->next == curr))){
            if (_xtest())
                _xabort(0xaa);
            else {
                ++num_tx_abort_forced;
                goto start_from_scratch_i;
            }
        }

        if (curr->key == x) {
			tx_end();
			return false;
		}
        else{
            newNode->next = curr;
			pred->next = newNode;
            tx_end();
            atomic_thread_fence(memory_order_release);
            return true;
        }
	}
	bool Remove(int x)
	{
		NODE* pred, * curr;
        int retry = -1;
	start_from_scratch_d:
        ++retry;

		pred = &head;
	    curr = pred->next;
		while (curr->key < x) {
			pred = curr; curr = curr->next;
		}

		atomic_thread_fence(memory_order_acquire);
        int status = tx_start();
        if (status != _XBEGIN_STARTED){
            goto start_from_scratch_d;
        }

        if(!((false == pred->removed) && (false == curr->removed) && (pred->next == curr))){
            if (_xtest())
                _xabort(0xaa);
            else {
                ++num_tx_abort_forced;
                goto start_from_scratch_d;
            }
        }

		if (curr->key != x) {
			tx_end();
			return false;
		}
		else {
			curr->removed = true;
			pred->next = curr->next;
			tx_end();
            atomic_thread_fence(memory_order_release);
            return true;
			
		}
	}

	bool Contains(int x)
	{
		NODE* curr;
		curr = &head;
		while (curr->key < x) {
			curr = curr->next;
		}

		return (false == curr->removed) && (x == curr->key);
	}
};


ZSET my_set;

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
