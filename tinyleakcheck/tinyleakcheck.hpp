#pragma once

/*
####################################################################################################

TinyLeakCheck
A tiny, standalone, thread-safe memory tracer and leak checker
by Agatha

https://github.com/geometrian/TinyLeakCheck

Basic usage:
	(1) Compile and link with "tinyleakcheck.cpp" (either as source or as a library, I don't care).
	(2) Call `TinyLeakCheck::prevent_linker_elison()` anywhere in your code (`#include` either this
	    file, "tinyleakcheck.hpp", or the minimal header "tiniestleakcheck.hpp").

There are no dependencies other than C++23 or newer (which you should be using anyway).  See the
readme for more discussion.

####################################################################################################

User can `#define` any of the following to configure:

	#define TINYLEAKCHECK_WHEN_ENABLED ⟨two-bit literal⟩
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

	#define TINYLEAKCHECK_PRETTIFY_STRS ⟨brace initializer of array of pairs of strings⟩
		Defines an array of (find,replace) pairs to be used internally for prettifying function
		names.  Note that actual `std::string`s can be used here without issue, if you prefer.  Also
		note that this must have been `#define`d when the "tinyleakcheck.cpp" file is compiled in
		order to have an effect!

	#define TINYLEAKCHECK_PRETTIFY_ENVS ⟨brace initializer of array of env. variables⟩
		Defines an array of environment variables to be searched for prettifying filenames.  If the
		variable is found in the name string, it is replaced by "%⟨varname⟩%".  Note that this must
		have been `#define`d when the "tinyleakcheck.cpp" file is compiled in order to have an
		effect!

	#define TINYLEAKCHECK_IGNORE_FUNCS ⟨brace initializer of array of function names⟩
		Defines an array of string that, if a leak triggers inside of a function whose name (after
		prettifying) contains any of them, will not "count" as a leak.  This is also used to handle
		certain memory allocations within the standard library that are cleaned up after static
		destruction (such an allocation would be falsely reported as a memory leak).  Note that this
		should *only* be used for ignoring functions in standard libraries; do *not* use this
		instead of fixing your code!

	#define TINYLEAKCHECK_ASSERT ⟨assert⟩
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
		{\
			{ "> >", ">>" },\
			{ "basic_string<char,std::char_traits<char>,std::allocator<char>>", "string" },\
			{ "basic_ifstream<char,std::char_traits<char>>", "ifstream" }\
		}
#endif
#ifndef TINYLEAKCHECK_PRETTIFY_ENVS
	#define TINYLEAKCHECK_PRETTIFY_ENVS\
		{ "VS2019INSTALLDIR" }
#endif
#ifndef TINYLEAKCHECK_IGNORE_STDLIB_FUNCS
	#define TINYLEAKCHECK_IGNORE_FUNCS { "std::use_facet", "std::_Facet_Register" }
#endif
#ifndef TINYLEAKCHECK_ASSERT
	#include <cassert>
	#define TINYLEAKCHECK_ASSERT( CHECK_EXPR, FMT_CSTR, ... ) assert(CHECK_EXPR)
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

#define TINYLEAKCHECK_PUSHABLE_DEPTH 8

#include <cstdarg>
#include <array>
#include <deque>
#include <map>
#include <stacktrace>
#include <string>
#include <thread>



#if defined __GNUC__ && !defined __clang__ //Ignore erroneous warnings from buggy GCC
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wattributes"
#endif

namespace TinyLeakCheck
{



//A helper datastructure (stack backed by array) used internally.  User does not need to care.
template< class T, std::size_t maxN >
class ArrayStack final
{
	private:
		std::array< T, maxN > _backing;
		std::size_t _count = 0;

	public:
		template< class... Ts > constexpr explicit
		ArrayStack( Ts const&... vals ) noexcept : _backing{ vals... }, _count(sizeof...(Ts))
		{
			static_assert( sizeof...(Ts) < maxN );
		}

		void clear() noexcept { _count=0; }

		T      & operator[]( std::size_t index )       noexcept
		{
			TINYLEAKCHECK_ASSERT( index<_count, "Index %zu out of bound!", index );
			return _backing[ (_count-1) - index ];
		}
		T const& operator[]( std::size_t index ) const noexcept
		{
			TINYLEAKCHECK_ASSERT( index<_count, "Index %zu out of bound!", index );
			return _backing[ (_count-1) - index ];
		}

		[[nodiscard]] constexpr bool        empty() const noexcept { return _count==0; }
		[[nodiscard]] constexpr std::size_t size () const noexcept { return _count; }

		[[nodiscard]] constexpr T      & peek()       noexcept
		{
			TINYLEAKCHECK_ASSERT( _count>0, "Stack contains no elements!" );
			return _backing[ _count - 1 ];
		}
		[[nodiscard]] constexpr T const& peek() const noexcept
		{
			TINYLEAKCHECK_ASSERT( _count>0, "Stack contains no elements!" );
			return _backing[ _count - 1 ];
		}

		void push( T const& val ) noexcept
		{
			TINYLEAKCHECK_ASSERT( _count<maxN, "Stack overflow!" );
			_backing[ _count++ ] = val;
		}
		T    pop (              ) noexcept
		{
			TINYLEAKCHECK_ASSERT( _count>0, "Stack underflow!" );
			return _backing[ --_count ];
		}
};



#ifdef TINYLEAKCHECK_ENABLED
//Memory tracer.  User does not need directly.
struct MemoryTracer final
{
	struct Mode final
	{
		//Whether we are tracing memory
		ArrayStack< bool, TINYLEAKCHECK_PUSHABLE_DEPTH > record;
		//Whether a stack trace should be recorded also.
		ArrayStack< bool, TINYLEAKCHECK_PUSHABLE_DEPTH > with_stacktrace;

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
	};
	Mode mode;

	//Represents a memory block.
	class BlockInfo final
	{
		friend struct MemoryTracer;
		public:
			void* ptr;
			size_t alignment, size;
			std::thread::id thread_id;
			std::stacktrace trace;
		private:
			bool mutable _finalized = false;
			std::string mutable _str;

		private:
			BlockInfo( void* ptr, size_t alignment,size_t size, bool with_stacktrace ) noexcept;

			[[nodiscard]] bool _finalize() const noexcept; //Returns whether block isn't ignored.

		public:
			void basic_print( FILE* file=stderr ) const noexcept;
	};
	//Map of pointers onto blocks.  User should not change, but is exposed to user.
	std::map< void*, BlockInfo* > blocks;

	//Callbacks.  User may set to override defaults.
	struct Callbacks
	{
		//Called by the default `.leaks_detected(⋯)`, for each block.  The default prettifies each
		//	frame if there is a stack track, then calls `.basic_print()` on the block.
		using PrintBlock = void(*)( MemoryTracer const& tracer, BlockInfo const& block );
		PrintBlock print_block;
		
		//Called immediately *after* each allocation.  Only enabled when recording is.  Default does
		//	nothing.
		using PostAlloc = void(*)(
			MemoryTracer const& tracer, void* ptr, size_t alignment, size_t size
		);
		PostAlloc post_alloc;

		//Called immediately *before* each deallocation.  Only enabled when recording is.  Default
		//	does nothing.
		using PreDealloc = void(*)(
			MemoryTracer const& tracer, void* ptr, size_t alignment
		);
		PreDealloc pre_dealloc;

		//Called if leaks are detected on program close.  The default prints a message, calls
		//	`.print_block(⋯)` on all blocks, and fails (trapping into debugger in debug mode) and
		//	then `std::abort()`s).
		using LeaksDetected = void(*)( MemoryTracer const& tracer );
		LeaksDetected leaks_detected;
	};
	Callbacks callbacks;

	MemoryTracer();
	~MemoryTracer();

	//Record an allocation / deallocation.  User does not need, but should be able to call with
	//	a custom memory allocator (e.g. to treat allocations within a pool as "real" allocations).
	void record_alloc  ( void* ptr, size_t alignment, size_t size );
	void record_dealloc( void* ptr, size_t alignment              );
};

//Per-thread memory tracer.  User does not need, but is exposed to the user.  Note may not exist
//	during static initialization!
extern MemoryTracer* memory_tracer;
#endif



//User needs to call in order to prevent the linker from eliding this module.  See also:
//	https://www.nsnam.org/docs/linker-problems.pdf
void prevent_linker_elison();



}

#if defined __GNUC__ && !defined __clang__
	#pragma GCC diagnostic pop
#endif
