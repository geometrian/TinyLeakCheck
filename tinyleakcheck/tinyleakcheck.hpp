#pragma once

/*
####################################################################################################

TinyLeakCheck
A tiny, standalone memory tracer and leak checker
by Agatha

Basic usage:
	(1) Compile and link with this (either as source or as a library, I don't care).
	(2) Call `TinyLeakCheck::prevent_linker_elison()` anywhere in your code (`#include` either this
	    file, "tinyleakcheck.hpp", or the minimal header "tiniestleakcheck.hpp").

There are no dependencies other than C++20 or newer (which you should be using anyway).  See the
readme for more discussion.

####################################################################################################

User can `#define` any of the following to configure:

	#define TINYLEAKCHECK_WHEN_ENABLED <two-bit literal>
		Code that determines when TinyLeakCheck is enabled.  The lower bit enables it in debug mode
		and the upper bit enables it in release mode.  E.g. `0b01` enables in debug, but not release
		mode (the default).  Note that this must have been `#define`d when the "tinyleakcheck.cpp"
		file is compiled in order to have an effect!

	#define TINYLEAKCHECK_NO_RECORD_ALLOCS_BY_DEFAULT
		Makes allocations not be recorded by default (presumably, you will push/pop
		`TinyLakeCheck::memory_tracer.mode.record` when you are ready to record stuff later).

	#define TINYLEAKCHECK_NO_STACK_TRACE_BY_DEFAULT
		For a recorded allocation, makes stack traced not be recorded by default.  (Similarly, you
		can change `TinyLakeCheck::memory_tracer.mode.with_stacktrace` to enable/disable later.)

	#define TINYLEAKCHECK_PRETTIFY_STRS <brace initializer of array of pairs of strings>
		Defines an array of (find,replace) pairs to be used internally for prettifying function
		names.  Note that actual `std::string`s can be used here without issue, if you prefer.  Also
		note that this must have been `#define`d when the "tinyleakcheck.cpp" file is compiled in
		order to have an effect!

	#define TINYLEAKCHECK_PRETTIFY_ENVS <brace initializer of array of env. variables>
		Defines an array of environment variables to be searched for prettifying filenames.  If the
		variable is found in the name string, it is replaced by "%<varname>%".  Note that this must
		have been `#define`d when the "tinyleakcheck.cpp" file is compiled in order to have an
		effect!  This flag only has an effect on Windows because filenames are only available there.

	#define TINYLEAKCHECK_ASSERT <assert>
		Defines an assertion function for TinyLeakCheck to use internally.  If none is provided, it
		uses `<cassert>`'s assert.  Note that this must be `#define`d when the "tinyleakcheck.cpp"
		file is compiled in order to have (complete) effect!

####################################################################################################
*/

#ifndef TINYLEAKCHECK_WHEN_ENABLED
	#define TINYLEAKCHECK_WHEN_ENABLED 0b01
#endif

#ifndef TINYLEAKCHECK_PRETTIFY_STRS
	#define TINYLEAKCHECK_PRETTIFY_STRS\
		{ {"> >",">>"}, {"basic_string<char,std::char_traits<char>,std::allocator<char>>","string"} }
#endif
#ifndef TINYLEAKCHECK_PRETTIFY_ENVS
	#define TINYLEAKCHECK_PRETTIFY_ENVS\
		{ "VS2019INSTALLDIR" }
#endif
#ifndef TINYLEAKCHECK_ASSERT
	#include <cassert>
	#define TINYLEAKCHECK_ASSERT(CHECK_EXPR,FMT_CSTR,...) assert(CHECK_EXPR)
#endif

#if   defined __i686__ || defined _M_IX86 //IA-32 (32-bit x86)
	#define TINYLEAKCHECK_IA_32
#elif defined __amd64__ || defined _M_AMD64 //AMD64 (64-bit x86-64)
	#define TINYLEAKCHECK_AMD64
#elif defined __ia64__ || defined _M_IA64 //IA-64 (Itanium)
	#define TINYLEAKCHECK_IA_64 //Note: not fully implemented on Windows!
#endif
#if !defined _WIN32 || ( defined TINYLEAKCHECK_IA_32 || defined TINYLEAKCHECK_AMD64 || defined TINYLEAKCHECK_IA_64 )
	#if (!defined NDEBUG && (TINYLEAKCHECK_WHEN_ENABLED&0b01)) || (defined NDEBUG && (TINYLEAKCHECK_WHEN_ENABLED&0b10))
		#define TINYLEAKCHECK_ENABLED //Can check leaks on non-Windows, or on Windows IA-32 / AMD64 / IA-64
	#endif
#endif

//##################################################################################################

#if defined __GNUC__ && !defined __clang__ //Ignore erroneous warnings from buggy GCC
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wattributes"
#endif

#define TINYLEAKCHECK_PUSHABLE_DEPTH 8

#include <cstdarg>
#include <array>
#include <deque>
#include <map>
#include <string>
#include <thread>

namespace TinyLeakCheck {

//A helper datastructure (stack backed by array) used internally.  User does not need to care.
template<class T,std::size_t maxN>
class ArrayStack final {
	private:
		std::array<T,maxN> _backing;
		std::size_t _count = 0;
	public:
		template<class... Ts> constexpr explicit ArrayStack(Ts const&... vals) noexcept : _backing{ vals... } { static_assert(sizeof...(Ts)<maxN); _count=sizeof...(Ts); }
		[[nodiscard]] constexpr T      & peek()       noexcept { TINYLEAKCHECK_ASSERT(_count>0,"Stack contains no elements!"); return _backing[_count-1]; }
		[[nodiscard]] constexpr T const& peek() const noexcept { TINYLEAKCHECK_ASSERT(_count>0,"Stack contains no elements!"); return _backing[_count-1]; }
		T      & operator[](std::size_t index)       noexcept { TINYLEAKCHECK_ASSERT(index<_count,"Index %zu out of bound!",index); return _backing[(_count-1)-index]; }
		T const& operator[](std::size_t index) const noexcept { TINYLEAKCHECK_ASSERT(index<_count,"Index %zu out of bound!",index); return _backing[(_count-1)-index]; }
		void clear() const noexcept { _count=0; }
		void push(T const& val) noexcept { TINYLEAKCHECK_ASSERT(_count<maxN,"Stack overflow!" ); _backing[_count++]=val;    }
		T    pop (            ) noexcept { TINYLEAKCHECK_ASSERT(_count>0   ,"Stack underflow!"); return _backing[--_count]; }
		[[nodiscard]] constexpr bool        empty() const noexcept { return _count==0; }
		[[nodiscard]] constexpr std::size_t size () const noexcept { return _count; }
};

//Describes a stack frame.  User does not need directly, but is exposed to the user.
struct StackFrame final {
	void* return_address;
	#ifdef _WIN32
		std::string module,name,filename; size_t line,line_offset;
	#else
		std::string function_identifier;
	#endif
	void prettify_strings(); //Replaces strings in `.name` and `.filename` to prettify output.  See discussion of macros above.
	void basic_print(FILE* file=stderr,size_t indent=4) const noexcept; //Basic print function used by default.
};

//Describes a stack trace.  User does not need directly, but is exposed to the user.  Constructing
//	this class anywhere builds a stack trace to that location, which the user may find handy!
struct StackTrace final {
	std::thread::id thread_id;
	std::deque<StackFrame> frames;
	StackTrace() noexcept;
	void pop(size_t count=1) noexcept { for (size_t k=0;k<count;++k) frames.pop_front(); } //pop `count` number of stack frames
	void basic_print(FILE* file=stderr,size_t indent=4) const noexcept; //Basic print function used by default.
};

//Memory tracer.  User does not need directly.
struct MemoryTracer final {
	struct Mode final {
		ArrayStack<bool,TINYLEAKCHECK_PUSHABLE_DEPTH> record;          //Whether we are tracing memory
		ArrayStack<bool,TINYLEAKCHECK_PUSHABLE_DEPTH> with_stacktrace; //Whether a stack trace should be recorded also.
		Mode() :
			#ifndef TINYLEAKCHECK_NO_RECORD_ALLOCS_BY_DEFAULT
			record(true ),
			#else
			record(false),
			#endif
			#ifndef TINYLEAKCHECK_NO_STACK_TRACE_BY_DEFAULT
			with_stacktrace(true )
			#else
			with_stacktrace(false)
			#endif
		{}
	} mode;

	//Represents a memory block.
	class BlockInfo final {
		friend struct MemoryTracer;
		public:
			void* ptr;
			size_t alignment, size;
			std::thread::id thread_id;
			StackTrace* trace;
		private:
			BlockInfo( void* ptr, size_t alignment,size_t size, bool with_stacktrace ) noexcept;
		public:
			~BlockInfo() { delete trace; }
			void basic_print(FILE* file=stderr,size_t indent=2) const noexcept;
	};
	//Map of pointers onto blocks.  User should not change, but is exposed to user.
	//	Note: if there are no blocks, this is null!
	std::map<void*,BlockInfo*>* blocks = nullptr;

	//Callbacks.  User may set to override defaults.
	struct {
		//Called by default `.leaks_detected(...)` on each block.  This default prettifies each
		//	frame if there is a stack track, then calls `.basic_print()` on the block.
		void(*print_block)(MemoryTracer const&,BlockInfo const&);
		//Called immediately *after* each allocation.  Only enabled when recording is.  Default does
		//	nothing.
		void(*post_alloc )(MemoryTracer const&,void*,size_t,size_t); //tracer, pointer, alignment, size (bytes)
		//Called immediately *before* each deallocation.  Only enabled when recording is.  Default
		//	does nothing.
		void(*pre_dealloc)(MemoryTracer const&,void*,size_t       ); //tracer, pointer, alignment
		//Called if leaks are detected on program close.  The default prints a message, calls
		//	`.print_block()` on all blocks, and fails (trapping into debugger in debug mode) and
		//	then `std::abort()`s).
		void(*leaks_detected)(MemoryTracer const&);
	} callbacks;

	MemoryTracer();
	~MemoryTracer();

	//Record an allocation / deallocation.  User does not need, but should be able to call with
	//	a custom memory allocator (e.g. to treat allocations within a pool as "real" allocations).
	void record_alloc  (void* ptr,size_t alignment,size_t size);
	void record_dealloc(void* ptr,size_t alignment            );
};

//Per-thread memory tracer.  User does not need, but is exposed to the user.
#ifdef TINYLEAKCHECK_ENABLED
extern thread_local MemoryTracer memory_tracer;
#endif

//User needs to call in order to prevent the linker from eliding this module.  See also:
//	https://www.nsnam.org/docs/linker-problems.pdf
void prevent_linker_elison();

}

#if defined __GNUC__ && !defined __clang__
	#pragma GCC diagnostic pop
#endif
