#pragma once
#include <mutex>
#include <memory>
#include <atomic>
#include <immintrin.h>

using namespace std;

atomic<int> g_num_tx_aborts{ 0 };
atomic<int> g_num_tx_abort_capacity{ 0 };
atomic<int> g_num_tx_abort_confilct{ 0 };
atomic<int> g_num_tx_abort_forced{ 0 };
atomic<int> g_num_tx_abort_explicit{ 0 };
atomic<int> g_num_tx_abort_rest{ 0 };

atomic<int> g_num_fallback{ 0 };

atomic<int> g_num_tx_commits{ 0 };



thread_local unsigned int num_tx_aborts = 0;
thread_local unsigned int num_tx_abort_capacity = 0;
thread_local unsigned int num_tx_abort_confilct = 0;
thread_local unsigned int num_tx_abort_forced = 0;
thread_local unsigned int num_tx_abort_explicit = 0;
thread_local unsigned int num_tx_abort_rest = 0;

thread_local unsigned int num_fallback = 0;

thread_local unsigned int num_tx_commits = 0;

thread_local int num_retry = 0;
#define FORCED_ABORT -10
#define MAX_RETRIES	10

int tx_start(mutex &lock, atomic_bool &is_locked)
{
	if(num_retry++ >= MAX_RETRIES) {
		lock.lock();
		is_locked.store(true);
		++num_fallback;
		return _XBEGIN_STARTED;
	}

	while(is_locked.load() == true);
    int status = _xbegin();
    if (_XBEGIN_STARTED == (unsigned)status) {
        return status;
    }
	if(is_locked.load() == true) _xabort(99);
    
	++num_tx_aborts;
    if (status & _XABORT_CAPACITY) {
        ++num_tx_abort_capacity;
	} else if (status & _XABORT_CONFLICT) {
        ++num_tx_abort_confilct;
    } else if (_XABORT_CODE(status) == 0x99) {
        ++num_tx_abort_forced;
        return FORCED_ABORT;
    } else if (status & _XABORT_EXPLICIT) {
        ++num_tx_abort_explicit;
	} else {
        ++num_tx_abort_rest;
	}

    return status;
}

int tx_end(mutex &lock, atomic_bool &is_locked)
{
	num_retry = 0;
	if(_xtest() == 1){
		_xend();
    	++num_tx_commits;
    	return 0;
	}
	is_locked.store(false);
    lock.unlock();
	return 0;
}

template <class T>
class ctr_block {
public:
	T* ptr;
	int ref_cnt;
	int weak_cnt;
};

template <class T>
class htm_shared_ptr {

private:
	template <class TF, class..._Types>
	friend htm_shared_ptr<TF> make_htm_shared(_Types&&... _Args);

	ctr_block<T>	*m_b_ptr;
	T* m_ptr;

	mutex lock;
	atomic_bool is_locked{false};
public:
	bool is_lock_free() const noexcept
	{
		return true;
	}

	void store(const htm_shared_ptr<T>& sptr, memory_order mem_order= memory_order_seq_cst) noexcept
	{
		bool need_delete = false;
		T* temp_ptr = nullptr;
		T* temp_b_ptr = nullptr;
		while (_XBEGIN_STARTED != tx_start(lock, is_locked));
		if (nullptr != m_b_ptr) {
			if (m_b_ptr->ref_count == 1) {
				need_delete = true;
				temp_ptr = m_ptr;
				temp_b_ptr = m_b_ptr;
			}
			m_b_ptr->ref_count--;
		}
		if (nullptr != sptr.m_ptr) {
			sptr.m_ptr->ref_count++;
		}
		m_ptr = sptr->m_ptr;
		m_b_ptr = sptr->m_b_ptr;
		tx_end(lock, is_locked);
		if (true == need_delete) {
			delete temp_ptr;
			delete temp_b_ptr;
		}
	}

	htm_shared_ptr<T> load(memory_order mem_order= memory_order_seq_cst) const noexcept
	{
		//while (_XBEGIN_STARTED != tx_start(lock, is_locked));
		htm_shared_ptr<T> t{ *this };
		//tx_end(lock, is_locked);
		return t;
	}

	//operator htm_shared_ptr<T>() const noexcept
	//{
	//	while (_XBEGIN_STARTED != tx_start(lock, is_locked));
	//	htm_shared_ptr<T> t {*this};
	//	tx_end(lock, is_locked);
	//	return t;
	//}

	htm_shared_ptr<T> exchange(htm_shared_ptr<T> &sptr, memory_order mem_order= memory_order_seq_cst) noexcept
	{
		while (_XBEGIN_STARTED != tx_start(lock, is_locked));
		ctr_block<T>* t_b = m_b_ptr;
		T* t_p = m_ptr;
		m_b_ptr = sptr.m_b_ptr;
		m_ptr = sptr.m_ptr;
		sptr.m_b_ptr = t_b;
        sptr.m_ptr = t_p;
		tx_end(lock, is_locked);
		return sptr;
	}

	bool compare_exchange_strong(const htm_shared_ptr<T>& expected_sptr, const htm_shared_ptr<T>& new_sptr, memory_order mem_order= memory_order_seq_cst) noexcept
	{
		bool success = false;
		bool need_delete = false;
		T* temp_ptr;
		ctr_block<T>* temp_b_ptr;
		while (_XBEGIN_STARTED != tx_start(lock, is_locked));
		if (m_b_ptr == expected_sptr.m_b_ptr) {
			if (nullptr != m_b_ptr) {
				if (m_b_ptr->ref_count == 1) {
					need_delete = true;
					temp_ptr = m_ptr;
					temp_b_ptr = m_b_ptr;
				}
				else if (m_b_ptr->ref_count < 1) _xabort(99);  // Fall Back Routine �߰� �ʿ�
				m_b_ptr->ref_cnt--;
			}
			m_ptr = new_sptr.m_ptr;
			m_b_ptr = new_sptr.m_b_ptr;
			new_sptr.m_b_ptr->ref_cnt++;
			success = true;
		}
		expected_sptr = m_ptr;
		tx_end(lock, is_locked);
		if (true == need_delete) {
			delete temp_ptr;
			delete temp_b_ptr;
		}
		return success;
	}

	bool compare_exchange_weak(const htm_shared_ptr<T>& expected_sptr, const htm_shared_ptr<T>& target_sptr, memory_order mem_order= memory_order_seq_cst) noexcept
	{
		return compare_exchange_strong(expected_sptr, target_sptr, mem_order);
	}

	htm_shared_ptr() noexcept
	{
		m_ptr = nullptr;
		m_b_ptr = nullptr;
	}

	~htm_shared_ptr() noexcept
	{
		bool need_delete = false;
		T *temp_ptr = nullptr;
		ctr_block<T> *temp_b_ptr = nullptr;
		while (_XBEGIN_STARTED != tx_start(lock, is_locked));
		if (nullptr != m_b_ptr) {
			if (m_b_ptr->ref_cnt == 1) {
				need_delete = true;
				temp_ptr = m_ptr;
				temp_b_ptr = m_b_ptr;
			}
			m_b_ptr->ref_cnt--;
		}
		tx_end(lock, is_locked);
		if (true == need_delete) {
			delete temp_ptr;
			delete temp_b_ptr;
		}
	}

	constexpr htm_shared_ptr(const htm_shared_ptr<T>& sptr) noexcept
	{
		while(_XBEGIN_STARTED != tx_start(lock, is_locked));
		m_ptr = sptr.m_ptr;
		m_b_ptr = sptr.m_b_ptr;
		if (nullptr != m_b_ptr) 
			m_b_ptr->ref_cnt++;
		tx_end(lock, is_locked);
	}
	//		htm_shared_ptr(const htm_shared_ptr&) = delete;
	//		htm_shared_ptr& operator=(const htm_shared_ptr&) = delete;
	htm_shared_ptr<T> operator=(const htm_shared_ptr<T>& sptr) noexcept
	{
		bool need_delete = false;
		T* temp_ptr;
		ctr_block<T>* temp_b_ptr;
		do {
			int t = tx_start(lock, is_locked);
			if (_XBEGIN_STARTED == t) break;
			if (99 == t) {
				m_ptr = nullptr;
				m_b_ptr = nullptr;
				return *this;
			}
		} while (true);

		if (nullptr != m_b_ptr) {
			if (m_b_ptr->ref_cnt == 1) {
				need_delete = true;
				temp_ptr = m_ptr;
				temp_b_ptr = m_b_ptr;
			}
			else if (m_b_ptr->ref_cnt < 1) _xabort(99);
			m_b_ptr->ref_cnt--;
		}
		m_ptr = sptr.m_ptr;
		m_b_ptr = sptr.m_b_ptr;
		m_b_ptr->ref_cnt++;
		tx_end(lock, is_locked);
		if (true == need_delete) {
			delete temp_ptr;
			delete temp_b_ptr;
		}
		return *this;
	}

	htm_shared_ptr<T> operator=(nullptr_t t) noexcept
	{
		reset();
		return *this;
	}


	T operator*() noexcept
	{
		T temp_ptr;
		while (_XBEGIN_STARTED != tx_start(lock, is_locked));
		if (0 < m_b_ptr->ref_cnt)
			temp_ptr = *m_ptr;
		tx_end(lock, is_locked);
		return temp_ptr;
	}

	void reset()
	{
		bool need_delete = false;
		T* temp_ptr;
		ctr_block<T>* temp_b_ptr;
		while (_XBEGIN_STARTED != tx_start(lock, is_locked));
		if (nullptr != m_b_ptr) {
			if (m_b_ptr->ref_cnt == 1) {
				need_delete = true;
				temp_ptr = m_ptr;
				temp_b_ptr = m_b_ptr;
			}
			// else if (m_b_ptr->ref_cnt < 1) _xabort();
			m_b_ptr->ref_cnt--;
		}
		m_ptr = nullptr;
		m_b_ptr = nullptr;
		tx_end(lock, is_locked);
		if (true == need_delete) {
			delete temp_ptr;
			delete temp_b_ptr;
		}
	}
	T* operator ->()
	{
		T* p = nullptr;
		bool exception = false;
		while (_XBEGIN_STARTED != tx_start(lock, is_locked));
		if (nullptr == m_b_ptr) exception = true;
		else if (m_b_ptr->ref_cnt < 1) exception = true;
		else p = m_ptr;
		tx_end(lock, is_locked);
		if (true == exception) {
            throw 99;
			//int* a = nullptr;
			//*a = 1;
		}
		return p;
	}


	//htm_shared_ptr(const htm_shared_ptr& rhs)
	//{
	//	store(rhs);
	//}
	//htm_shared_ptr& operator=(const htm_shared_ptr& rhs)
	//{
	//	store(rhs);
	//	return *this;
	//}
	template< typename TargetType >
	inline bool operator ==(const htm_shared_ptr< TargetType >& rhs)
	{
		return m_b_ptr == rhs.m_b_ptr;
	}
};


template <class T, class..._Types>
htm_shared_ptr<T> make_htm_shared(_Types&&... _Args)
{
	T* temp = new T{ forward<_Types>(_Args)... };
	htm_shared_ptr<T> new_sp;
	new_sp.m_ptr = temp;
	new_sp.m_b_ptr = new ctr_block <T>;
	new_sp.m_b_ptr->ptr = temp;
	new_sp.m_b_ptr->ref_cnt = 1;
	new_sp.m_b_ptr->weak_cnt = 0;
	return new_sp;
}
