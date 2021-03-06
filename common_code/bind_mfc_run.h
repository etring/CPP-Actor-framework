#ifndef __BIND_MODAL_RUN_H
#define __BIND_MODAL_RUN_H

#include <boost/function.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <boost/bind.hpp>
#include <list>
#include "wrapped_post_handler.h"
#include "actor_framework.h"
#include "mfc_strand.h"

using namespace std;

#define 	WM_USER_BEGIN		(WM_USER+0x8000)
#define 	WM_USER_POST		(WM_USER+0x8001)
#define 	WM_USER_SEND		(WM_USER+0x8002)
#define		WM_USER_END			(WM_USER+0x8003)

#define		BIND_MFC_RUN(__base__) \
private:\
void post_message(int id)\
{\
	PostMessage(id);\
}\
\
void peek_message()\
{\
	MSG msg;\
	while (PeekMessage(&msg, m_hWnd, WM_USER_BEGIN, WM_USER_END, PM_REMOVE))\
	{}\
}\
void cancel()\
{\
	__base__::OnCancel();\
}\
\
LRESULT _postRun(WPARAM wp, LPARAM lp)\
{\
	return bind_mfc_run::_postRun(wp, lp);\
}\
\
LRESULT _sendRun(WPARAM wp, LPARAM lp)\
{\
	return bind_mfc_run::_sendRun(wp, lp);\
}\
\
afx_msg void OnCancel();

#define REGIEST_MFC_RUN(__dlg__) \
ON_WM_CLOSE()\
ON_MESSAGE(WM_USER_POST, &__dlg__::_postRun)\
ON_MESSAGE(WM_USER_SEND, &__dlg__::_sendRun)

class bind_mfc_run
{
protected:
	struct bind_run
	{
		virtual void run() = 0;
	};

	template <typename T = void>
	struct bind_run_pck: public bind_run
	{
		bind_run_pck() {}

		bind_run_pck(const boost::function<void (T)>& cb, const boost::function<T ()>& h)
			: _cb(cb), _h(h) {}

		void run()
		{
			_cb(_h());
		}

		boost::function<void (T)> _cb;
		boost::function<T ()> _h;
	};

	template <>
	struct bind_run_pck<void>: public bind_run
	{
		bind_run_pck() {}

		bind_run_pck(const boost::function<void ()>& cb, const boost::function<void ()>& h)
			: _cb(cb), _h(h) {}

		void run()
		{
			_h();
			_cb();
		}

		boost::function<void ()> _cb;
		boost::function<void ()> _h;
	};
public:
	bind_mfc_run(): _isClosed(false) {};
	~bind_mfc_run() {};
public:
	/*!
	@brief 绑定一个函数到MFC队列执行
	*/
	template <typename Handler>
	wrapped_post_handler<bind_mfc_run, Handler> wrap(const Handler& handler)
	{
		return wrapped_post_handler<bind_mfc_run, Handler>(this, handler);
	}
protected:
	virtual void post_message(int id) = 0;
	virtual void peek_message() = 0;
	virtual void cancel() = 0;

	void clear_message()
	{
		boost::unique_lock<boost::shared_mutex> ul(_postMutex);
		peek_message();
		_postOptions.clear();
		_sendOptions.clear();
	}

	void mfc_close()
	{
		_isClosed = true;
		clear_message();
		cancel();
	}
public:
	/*!
	@brief 在MFC主线程初始化时调用
	*/
	void set_thread_id()
	{
		_threadID = boost::this_thread::get_id();
	}

	/*!
	@brief 获取主线程ID
	*/
	boost::thread::id thread_id()
	{
		assert(boost::thread::id() != _threadID);
		return _threadID;
	}

	/*!
	@brief 发送一个执行函数到MFC消息队列中执行
	*/
	void post(const boost::function<void ()>& h)
	{
		boost::shared_lock<boost::shared_mutex> sl(_postMutex);
		if (!_isClosed)
		{
			_mutex1.lock();
			_postOptions.push_back(h);
			_mutex1.unlock();
			post_message(WM_USER_POST);
		}
	}
	
	/*!
	@brief 发送一个执行函数到MFC消息队列中执行，完成后返回
	*/
	void send(boost_actor* actor, const boost::function<void ()>& h)
	{
		assert(boost::this_thread::get_id() != thread_id());
		actor->trig([&, this](const boost::function<void ()>& cb)
		{
			boost::shared_lock<boost::shared_mutex> sl(_postMutex);
			if (!_isClosed)
			{
				boost::shared_ptr<bind_mfc_run::bind_run_pck<> > pck(new bind_mfc_run::bind_run_pck<>(cb, h));
				_mutex2.lock();
				_sendOptions.push_back(pck);
				_mutex2.unlock();
				this->post_message(WM_USER_SEND);
			}
		});
	}

	/*!
	@brief 发送一个带返回值函数到MFC消息队列中执行，完成后返回
	*/
	template <typename T>
	T send(boost_actor* actor, const boost::function<T ()>& h)
	{
		assert(boost::this_thread::get_id() != thread_id());
		return actor->trig<T>([&, this](const boost::function<void (T)>& cb)
		{
			boost::shared_lock<boost::shared_mutex> sl(_postMutex);
			if (!_isClosed)
			{
				boost::shared_ptr<bind_mfc_run::bind_run_pck<T> > pck(new bind_mfc_run::bind_run_pck<T>(cb, h));
				_mutex2.lock();
				_sendOptions.push_back(pck);
				_mutex2.unlock();
				this->post_message(WM_USER_SEND);
			}
		});
	}

#ifdef ENABLE_MFC_ACTOR
	/*!
	@brief 在MFC线程中创建一个Actor
	@param ios Actor内部timer使用的调度器，没有就不能用timer
	*/
	actor_handle create_mfc_actor(ios_proxy& ios, const boost_actor::main_func& mainFunc, size_t stackSize = DEFAULT_STACKSIZE)
	{
		return boost_actor::create(mfc_strand::create(ios, this), mainFunc, stackSize);
	}

	actor_handle create_mfc_actor(const boost_actor::main_func& mainFunc, size_t stackSize = DEFAULT_STACKSIZE)
	{
		return boost_actor::create(mfc_strand::create(this), mainFunc, stackSize);
	}
#endif
protected:
	LRESULT _postRun(WPARAM wp, LPARAM lp)
	{
		_mutex1.lock();
		assert(!_postOptions.empty());
		boost::function<void ()> h = _postOptions.front();
		_postOptions.pop_front();
		_mutex1.unlock();
		assert(h);
		h();
		return 0;
	}

	LRESULT _sendRun(WPARAM wp, LPARAM lp)
	{
		_mutex2.lock();
		assert(!_sendOptions.empty());
		boost::shared_ptr<bind_run> pck = _sendOptions.front();
		_sendOptions.pop_front();
		_mutex2.unlock();
		pck->run();
		return 0;
	}
private:
	list<boost::function<void ()> > _postOptions;
	list<boost::shared_ptr<bind_run> > _sendOptions;
	boost::mutex _mutex1;
	boost::mutex _mutex2;
	boost::shared_mutex _postMutex;
	boost::thread::id _threadID;
	bool _isClosed;
};

#endif