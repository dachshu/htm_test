#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>

using namespace std;
using namespace std::chrono;

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

constexpr int MAXHEIGHT = 10;
class SLNODE {
public:
	int key;
	SLNODE* next[MAXHEIGHT];
	int height;
	SLNODE(int x, int h)
	{
		key = x;
		height = h;
		for (auto& p : next) p = nullptr;
	}
	SLNODE(int x)
	{
		key = x;
		height = MAXHEIGHT;
		for (auto& p : next) p = nullptr;
	}
	SLNODE()
	{
		key = 0;
		height = MAXHEIGHT;
		for (auto& p : next) p = nullptr;
	}
};

class SKLIST {
	SLNODE head, tail;
	mutex glock;
public:
	SKLIST()
	{
		head.key = 0x80000000;
		tail.key = 0x7FFFFFFF;
		head.height = tail.height = MAXHEIGHT;
		for (auto& p : head.next) p = &tail;
	}
	~SKLIST() {
		Init();
	}

	void Init()
	{
		SLNODE* ptr;
		while (head.next[0] != &tail) {
			ptr = head.next[0];
			head.next[0] = head.next[0]->next[0];
			delete ptr;
		}
		for (auto& p : head.next) p = &tail;
	}
	void Find(int key, SLNODE* preds[MAXHEIGHT], SLNODE* currs[MAXHEIGHT])
	{
		int cl = MAXHEIGHT - 1;
		while (true) {
			if (MAXHEIGHT - 1 == cl)
				preds[cl] = &head;
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

	bool Add(int key)
	{
		SLNODE* preds[MAXHEIGHT], * currs[MAXHEIGHT];

		glock.lock();
		Find(key, preds, currs);

		if (key == currs[0]->key) {
			glock.unlock();
			return false;
		}
		else {
			int height = 1;
			while (fast_rand() % 2 == 0) {
				height++;
				if (MAXHEIGHT == height) break;
			}
			SLNODE* node = new SLNODE(key, height);
			for (int i = 0; i < height; ++i) {
				preds[i]->next[i] = node;
				node->next[i] = currs[i];
			}

			glock.unlock();
			return true;
		}
	}
	bool Remove(int key)
	{
		SLNODE* preds[MAXHEIGHT], * currs[MAXHEIGHT];

		glock.lock();
		Find(key, preds, currs);

		if (key == currs[0]->key) {
			for (int i = 0; i < currs[0]->height; ++i) {
				preds[i]->next[i] = currs[i]->next[i];
			}
			delete currs[0];
			glock.unlock();
			return true;
		}
		else {
			glock.unlock();
			return false;
		}
	}
	bool Contains(int key)
	{
		SLNODE* preds[MAXHEIGHT], * currs[MAXHEIGHT];
		glock.lock();
		Find(key, preds, currs);
		if (key == currs[0]->key) {
			glock.unlock();
			return true;
		}
		else {
			glock.unlock();
			return false;
		}
	}

	void display20()
	{
		int c = 20;
		SLNODE* p = head.next[0];
		while (p != &tail)
		{
			cout << p->key << ", ";
			p = p->next[0];
			c--;
			if (c == 0) break;
		}
		cout << endl;
	}
};


const auto NUM_TEST = 4000000;
const auto KEY_RANGE = 1000;
SKLIST list;
void ThreadFunc(int num_thread)
{
	int key;

	for (int i = 0; i < NUM_TEST / num_thread; i++) {
		switch (fast_rand() % 3) {
		case 0: key = fast_rand() % KEY_RANGE;
			list.Add(key);
			break;
		case 1: key = fast_rand() % KEY_RANGE;
			list.Remove(key);
			break;
		case 2: key = fast_rand() % KEY_RANGE;
			list.Contains(key);
			break;
		default: cout << "Error\n";
			exit(-1);
		}
	}
}

int main()
{
	for (auto n = 1; n <= 16; n *= 2) {
		list.Init();
		vector <thread> threads;
		auto s = high_resolution_clock::now();
		for (int i = 0; i < n; ++i)
			threads.emplace_back(ThreadFunc, n);
		for (auto& th : threads) th.join();
		auto d = high_resolution_clock::now() - s;
		list.display20();
		cout << n << "Threads,  ";
		cout << ",  Duration : " << duration_cast<milliseconds>(d).count() << " msecs.\n";
	}
	system("pause");
}


