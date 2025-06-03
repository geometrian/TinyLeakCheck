#if defined _WIN32 && !defined _CRT_SECURE_NO_WARNINGS
	//No, Microsoft :P  The standard library is *not* "deprecated" . . .
	#define _CRT_SECURE_NO_WARNINGS
#endif

#include "tinyleakcheck.hpp"

#ifdef _WIN32
	#include <Windows.h>
	#include <DbgHelp.h>
	#undef IGNORE
	#pragma comment(lib, "dbghelp.lib")
#else
	#include <cstring>
	#include <execinfo.h>
#endif

#include <bit>
#include <format>
#include <mutex>
#include <sstream>
#include <vector>

#if defined __GNUC__ && !defined __clang__

//Ignore erroneous warnings from buggy GCC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"

#ifndef __cpp_lib_bit_cast

//Fix the `std::bit_cast<⋯>(⋯)` they don't actually have.  If you don't like me putting stuff in
//	`std::`, then you should have implemented the functionality you were supposed to :)
namespace std
{

template< class TypeTo, class TypeFrom > inline static
TypeTo bit_cast( TypeFrom const& from ) noexcept
{
	static_assert( sizeof(TypeTo)==sizeof(TypeFrom), "Size mismatch!" );

	TypeTo result;
	memcpy( &result,&from, sizeof(TypeTo) ); //Can't be `constexpr` :(
	return result;
}

}

#endif

#endif



namespace TinyLeakCheck
{



#ifdef _WIN32

inline static void* aligned_malloc( std::size_t alignment, std::size_t size ) noexcept
{
	return _aligned_malloc( size, alignment );
}
inline static void aligned_free( void* ptr ) noexcept
{
	_aligned_free(ptr);
}

#else

//Note that for `std::aligned_alloc(⋯)`, the size is required to be a multiple of the alignment.
//	We could manually pad it out:
#if 0
inline static void* aligned_malloc( std::size_t alignment, std::size_t size ) noexcept
{
	size += ( alignment - size%alignment )%alignment;
	return std::aligned_alloc( alignment, size );
}
inline static void aligned_free( void* ptr ) noexcept { free(ptr); }
#endif
/*
. . . however unfortunately apparently on some glibc-based platforms, there is a bug in certain
versions that causes memory corruption.  I have validated this extensively and in any case other
people have noticed this in other contexts since at least 2016---yet even on up-to-date systems the
bug persists.  Simply amazing.  Instead, basically implement aligned malloc and free ourselves :V

TODO: recheck.  In some documentations, `std::aligned_alloc(⋯)` had swapped arguments?
*/
using TypeOffset = uint16_t;
static void* aligned_malloc( std::size_t alignment, std::size_t size ) noexcept
{
	TINYLEAKCHECK_ASSERT(
		alignment <= std::numeric_limits<TypeOffset>::max(),
		"Alignment %zu too large!  Use a smaller alignment or increase offset width.",
		alignment
	);

	std::size_t count = alignment-1 + sizeof(TypeOffset) + size;
	uint8_t* byteptr = static_cast<uint8_t*>( malloc(count) );

	size_t offset = alignment - std::bit_cast<uintptr_t>(byteptr)%alignment;
	if ( offset < sizeof(TypeOffset) ) offset += alignment;
	TINYLEAKCHECK_ASSERT(
		offset>=sizeof(TypeOffset) && offset+size<=count,
		"Implementation error!"
	);

	byteptr += offset;

	TypeOffset offsettmp = static_cast<TypeOffset>(offset);
	memcpy( byteptr-sizeof(TypeOffset),&offsettmp, sizeof(TypeOffset) );

	return byteptr;
}
inline static void aligned_free( void* ptr ) noexcept
{
	uint8_t* byteptr = static_cast<uint8_t*>(ptr);

	TypeOffset offset;
	memcpy( &offset,byteptr-sizeof(TypeOffset), sizeof(TypeOffset) );

	byteptr -= offset;

	free(byteptr);
}

#endif



static void str_replace(
	std::string* str_input,
	std::string_view str_to_find,
	std::string_view str_to_replace_with
) {
	size_t len_find = str_to_find.length();
	if ( len_find == 0 ) [[unlikely]] return;

	size_t len_repl = str_to_replace_with.length();

	size_t pos = 0;
	for ( ; (pos=str_input->find(str_to_find,pos))!=std::string::npos; )
	{
		str_input->replace( pos,len_find, str_to_replace_with );
		pos += len_repl;
	}
}
[[nodiscard]] inline static std::string str_get_replaced(
	std::string_view str_input,
	std::string_view str_to_find,
	std::string_view str_to_replace_with
) {
	std::string tmp(str_input);
	str_replace( &tmp, str_to_find,str_to_replace_with );
	return tmp;
}
[[nodiscard]] inline static bool str_contains(
	std::string_view str, std::string_view other
) noexcept
{
	return str.find(other) != std::string::npos;
}



#ifdef TINYLEAKCHECK_ENABLED
MemoryTracer::BlockInfo::BlockInfo(
	void* ptr, size_t alignment,size_t size, bool with_stacktrace
) noexcept :
	ptr(ptr), alignment(alignment),size(size), thread_id(std::this_thread::get_id())
{
	if ( !with_stacktrace ) [[unlikely]]
	{
		//Stack trace was not requested.  This improves performance and is actually necessary e.g.
		//	when generating the stack trace itself (we'd get infinite recursion).
		return;
	}

	trace = std::stacktrace( std::stacktrace::current() );
}

[[nodiscard]] bool MemoryTracer::BlockInfo::_finalize() const noexcept
{
	bool keep = true;

	struct PrettifyReplacement final { std::string find, replace; };
	PrettifyReplacement const replacements[] = TINYLEAKCHECK_PRETTIFY_STRS;
	std::string const ignore_funcs[] = TINYLEAKCHECK_IGNORE_FUNCS;

	//Leak record
	_str = std::format(
		"  Leaked {:p} ( align {}, size {}, thread {} )",
		ptr,
		alignment, size, thread_id
	);

	/*
	Stack trace
		We need to pop four times to get to the call site where the memory was allocated:
			MemoryTracer::BlockInfo::BlockInfo(⋯)
			MemoryTracer::record_alloc(⋯)
			_alloc(⋯)
			operator new(⋯) / operator new[](⋯)
	*/
	if ( trace.size() <= 4 ) [[unlikely]]
	{
		_str += '\n';
	}
	else
	{
		_str += " allocated at:\n";
		for ( size_t iframe=4; iframe<trace.size(); ++iframe )
		{
			std::stacktrace_entry const& frame = trace[iframe];

			std::string descr = frame.description();
			for ( PrettifyReplacement const& replacement : replacements )
			{
				descr = str_get_replaced( descr, replacement.find,replacement.replace );
			}
			if (keep)
			{
				for ( std::string const& ignore_func : ignore_funcs )
				{
					if ( str_contains( descr, ignore_func ) )
					{
						keep = false;
						break;
					}
				}
			}

			std::string frame_str = "    ";

			//frame_str += module;

			if ( !descr.empty() ) [[likely]] frame_str += descr;
			else
			{
				//frame_str += std::to_string(return_address);
				frame_str += "<unknown>";
			}

			std::string filename = frame.source_file();
			if ( filename.empty() ) [[unlikely]] frame_str += '\n';
			else
			{
				//Take shortest replacement
				std::string shortest_filename = filename;
				for ( char const* varname : TINYLEAKCHECK_PRETTIFY_ENVS )
				{
					char const* var = std::getenv(varname);
					if ( var == nullptr ) continue;
					std::string repl = str_get_replaced(
						filename, var,std::string("%")+varname+"%"
					);
					if ( repl.length() < shortest_filename.length() ) shortest_filename = repl;
				}
				filename = shortest_filename;

				frame_str += " at ";
				frame_str += filename;
				uint64_t line_no = static_cast<uint64_t>( frame.source_line() );
				if ( line_no != 0 )
				{
					frame_str += std::format( "({})", line_no );
					//"(%zu,%zu)\n", line,line_offset; //TODO somehow?
				}
				frame_str += '\n';
			}

			_str += frame_str;
		}
	}

	_finalized = true;

	return keep;
}

void MemoryTracer::BlockInfo::basic_print( FILE* file/*=stderr*/ ) const noexcept
{
	if ( !_finalized ) (void)_finalize();

	fprintf( file, "%s", _str.c_str() );
}



#ifdef __clang__
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wexit-time-destructors"
	#pragma clang diagnostic ignored "-Wglobal-constructors"
#endif
static std::recursive_mutex _memory_tracer_mutex;
#ifdef __clang__
	#pragma clang diagnostic pop
#endif
static void _default_callback_print_block(
	MemoryTracer const& /*tracer*/, MemoryTracer::BlockInfo const& block
) {
	block.basic_print();
}
static void _default_callback_post_alloc (
	MemoryTracer const& /*tracer*/, void* /*ptr*/, size_t /*alignment*/, size_t /*size*/
) {}
static void _default_callback_pre_dealloc(
	MemoryTracer const& /*tracer*/, void* /*ptr*/, size_t /*alignment*/
) {}
[[noreturn]] static void _default_callback_leaks_detected( MemoryTracer const& tracer )
{
	fprintf( stderr, "Leaks detected!\n" );
	for ( auto const& iter : tracer.blocks )
	{
		tracer.callbacks.print_block( tracer, *iter.second );
	}

	/*
	Welcome, humble programmer!  I have summoned you here today to help you debug your code.  If you
	are reading this, you probably have been trapped here by your debugger.  Fear not!  The details
	of the memory leaks in your code have (by default) been dumped directly into `stderr`!  Fix it!
	*/
	#ifndef NDEBUG
		#if   defined __clang__ || defined __GNUC__
			__builtin_trap();
		#elif defined _MSC_VER
			__debugbreak();
		#endif
	#endif

	#ifdef __clang__
		#pragma clang diagnostic push
		#pragma clang diagnostic ignored "-Wunreachable-code"
	#endif
	std::abort();
	#ifdef __clang__
		#pragma clang diagnostic pop
	#endif
}

MemoryTracer::MemoryTracer()
{
	callbacks.print_block    = _default_callback_print_block   ;
	callbacks.post_alloc     = _default_callback_post_alloc    ;
	callbacks.pre_dealloc    = _default_callback_pre_dealloc   ;
	callbacks.leaks_detected = _default_callback_leaks_detected;
}
MemoryTracer::~MemoryTracer()
{
	if ( blocks.empty() ) [[likely]] return;

	memory_tracer->mode.record.push(false); //and don't bother to pop later

	//Final processing on all blocks, calculating prettified strings and removing those which
	//	should be ignored.
	for ( auto iter=blocks.cbegin(); iter!=blocks.cend(); )
	{
		BlockInfo const* block = iter->second;

		bool keep = block->_finalize();
		if (keep) [[unlikely]]
		{
			++iter;
		}
		else
		{
			auto iter_old = iter++;
			blocks.erase(iter_old);
			delete block;
		}
	}

	if ( blocks.empty() ) [[likely]] return;

	//If there are still blocks, then memory leak!

	callbacks.leaks_detected(*this);

	for ( auto iter : blocks )
	{
		BlockInfo* block = iter.second;
		delete iter.second;
	}
	//blocks.clear();
}

void MemoryTracer::record_alloc  ( void* ptr, size_t alignment, size_t size )
{
	std::lock_guard<std::recursive_mutex> lock_raii(_memory_tracer_mutex);

	if ( !mode.record.peek() ) return;

	mode.record.push(false);

	blocks.emplace(
		ptr,
		new BlockInfo( ptr, alignment, size, mode.with_stacktrace.peek() )
	);

	callbacks.post_alloc( *this, ptr, alignment, size );

	mode.record.pop();
}
void MemoryTracer::record_dealloc( void* ptr, size_t alignment              )
{
	if ( ptr == nullptr ) return;

	std::lock_guard<std::recursive_mutex> lock_raii(_memory_tracer_mutex);

	if ( !mode.record.peek() ) return;

	mode.record.push(false);

	callbacks.pre_dealloc( *this, ptr, alignment );

	auto iter = blocks.find(ptr);
	TINYLEAKCHECK_ASSERT( iter!=blocks.cend(), "Deleting an invalid pointer 0x%p!", ptr );
	delete iter->second;
	blocks.erase(iter);

	mode.record.pop();
}



/*
When a `MemoryTracer` exists, it can record allocations, and when it's deleted it can report the
allocations that weren't freed as memory leaks.  So when should the instance `memory_tracer` be
created and destroyed?

We can't tie it closely to the `operator new(⋯)` / `operator delete(⋯)` functions, because the
whole point is that these might be mismatched.  Another alternative is to make the user allocate /
deallocate it.  This is inconvenient, and of course the user is trying to debug mismatched
allocation / deallocation in the first place.  The right solution is to make it a namespace-scope
variable.

Note, however, that this solution (along with the previous, as it happens) has a subtle problem:
allocation can happen during normal runtime, but get freed by the standard library after the fact,
causing a false positive.  A (real) example is something like:
	(1) Static construction of `memory_tracer`.
	(2) Beginning of `main(⋯)`.
	(3) User code does file IO, standard library allocates memory.
	(4) End of `main(⋯)`.
	(5) Static destruction of `memory_tracer`, leaks reported!
	(6) Standard library cleans up allocated memory.
This problem cannot be solved "correctly".  The code is functionally correct, and there's no way to
detect this case.  Since the latest we can run code is with static destructors, we have no way of
knowing whether the standard library does indeed clean up, or whether it represents a leak.

At the same time, constructing the memory leak detector at namespace scope allows us to detect (at
least some) static memory leaks, which is very useful.  So, we still do this.  The false positives
are explicitly ignored.
*/
MemoryTracer* memory_tracer = nullptr;
static bool _ready = false;
inline static void* _alloc  ( size_t alignment, size_t size )
{
	void* result = aligned_malloc( alignment, size );
	if (_ready) [[likely]] memory_tracer->record_alloc( result, alignment, size );
	return result;
}
inline static void  _dealloc( size_t alignment, void* ptr   )
{
	if (_ready) [[likely]] memory_tracer->record_dealloc( ptr, alignment );
	aligned_free(ptr);
}
struct _EnsureMemoryTracer final
{
	_EnsureMemoryTracer()
	{
		memory_tracer = new MemoryTracer;
		_ready = true;
	}
	~_EnsureMemoryTracer() noexcept
	{
		_ready = false;
		delete memory_tracer;
		memory_tracer = nullptr;
	}
};
#ifdef __clang__
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wexit-time-destructors"
	#pragma clang diagnostic ignored "-Wglobal-constructors"
#endif
static _EnsureMemoryTracer _ensure_memory_tracer;
#ifdef __clang__
	#pragma clang diagnostic pop
#endif
#endif

void prevent_linker_elison() {}

}

#ifdef TINYLEAKCHECK_ENABLED

[[nodiscard]] void* operator new( std::size_t size                             )
{
	return TinyLeakCheck::_alloc( __STDCPP_DEFAULT_NEW_ALIGNMENT__, size );
}
[[nodiscard]] void* operator new( std::size_t size, std::align_val_t alignment )
{
	return TinyLeakCheck::_alloc( static_cast<size_t>(alignment)  , size );
}

void operator delete( void* ptr                             ) noexcept
{
	TinyLeakCheck::_dealloc( __STDCPP_DEFAULT_NEW_ALIGNMENT__, ptr );
}
void operator delete( void* ptr, std::align_val_t alignment ) noexcept
{
	TinyLeakCheck::_dealloc( static_cast<size_t>(alignment)  , ptr );
}

#endif

#if defined __GNUC__ && !defined __clang__

#pragma GCC diagnostic pop

#endif
