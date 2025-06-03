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

#include <mutex>
#include <sstream>
#include <vector>

#if defined __GNUC__ && !defined __clang__

//Ignore erroneous warnings from buggy GCC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"

#ifndef __cpp_lib_bit_cast

//Fix the `std::bit_cast<...>(...)` they don't actually have.  If you don't like me putting stuff in
//	`std::`, then you should have implemented the functionality you were supposed to :)
namespace std {

template<class TypeTo,class TypeFrom> inline static TypeTo bit_cast(TypeFrom const& from) noexcept {
	static_assert(sizeof(TypeTo)==sizeof(TypeFrom),"Size mismatch!");
	TypeTo result; memcpy(&result,&from,sizeof(TypeTo)); //Can't be `constexpr` :(
	return result;
}

}

#endif

#endif

namespace TinyLeakCheck {

#ifdef _WIN32
inline static void* aligned_malloc(std::size_t alignment,std::size_t size) noexcept {
	return _aligned_malloc(size,alignment);
}
inline static void aligned_free(void* ptr) noexcept {
	_aligned_free(ptr);
}
#else
//Note that for `std::aligned_alloc(...)`, the size is required to be a multiple of the alignment.
//	We could manually pad it out:
#if 0
inline static void* aligned_malloc(std::size_t alignment,std::size_t size) noexcept {
	size += ( alignment - size%alignment )%alignment;
	return std::aligned_alloc(alignment,size);
}
inline static void aligned_free(void* ptr) noexcept { free(ptr); }
#endif
/*
. . . however unfortunately apparently on some glibc-based platforms, there is a bug in certain
versions that causes memory corruption.  I have validated this extensively and in any case other
people have noticed this in other contexts since at least 2016---yet even on up-to-date systems
the bug persists.  Simply amazing.  Instead, basically implement aligned malloc and free
ourselves :V

TODO: recheck.  In some documentations, `std::aligned_alloc(...)` had swapped arguments?
*/
using TypeOffset = uint16_t;
inline static void* aligned_malloc(std::size_t alignment,std::size_t size) noexcept {
	TINYLEAKCHECK_ASSERT(alignment<=std::numeric_limits<TypeOffset>::max(),"Alignment too large!  Use a smaller alignment or increase offset width.");

	std::size_t count = alignment-1 + sizeof(TypeOffset) + size;
	uint8_t* byteptr = static_cast<uint8_t*>(malloc(count));

	size_t offset = alignment - std::bit_cast<uintptr_t>(byteptr)%alignment;
	if (offset<sizeof(TypeOffset)) offset+=alignment;
	TINYLEAKCHECK_ASSERT(offset>=sizeof(TypeOffset)&&offset+size<=count,"Implementation error!");

	byteptr += offset;

	TypeOffset offsettmp = static_cast<TypeOffset>(offset);
	memcpy(byteptr-sizeof(TypeOffset),&offsettmp,sizeof(TypeOffset));

	return byteptr;
}
inline static void aligned_free(void* ptr) noexcept {
	uint8_t* byteptr = static_cast<uint8_t*>(ptr);

	TypeOffset offset; memcpy(&offset,byteptr-sizeof(TypeOffset),sizeof(TypeOffset));

	byteptr -= offset;

	free(byteptr);
}
#endif

static void str_replace( std::string* str_input, std::string_view const& str_to_find,std::string_view const& str_to_replace_with ) {
	size_t len_find = str_to_find.length();
	if (len_find==0) [[unlikely]] return;

	size_t len_repl = str_to_replace_with.length();

	size_t pos = 0;
	for (;(pos=str_input->find(str_to_find,pos))!=std::string::npos;) {
		str_input->replace( pos,len_find, str_to_replace_with );
		pos += len_repl;
	}
}
[[nodiscard]] inline static std::string str_get_replaced( std::string const& str_input, std::string_view const& str_to_find,std::string_view const& str_to_replace_with ) {
	std::string tmp=str_input; str_replace( &tmp, str_to_find,str_to_replace_with ); return tmp;
}
[[nodiscard]] inline static bool str_contains(std::string const& str,std::string const& other) noexcept {
	return str.find(other) != std::string::npos;
}

#ifdef _WIN32
struct MutexWrapper final {
	std::mutex mutex;
	static std::atomic_bool ready;
	MutexWrapper() { MutexWrapper::ready=true; }
};
std::atomic_bool MutexWrapper::ready = false;
#ifdef __clang__
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wexit-time-destructors"
	#pragma clang diagnostic ignored "-Wglobal-constructors"
#endif
static MutexWrapper _stacktrace_mutex;
#ifdef __clang__
	#pragma clang diagnostic pop
#endif
#endif

bool StackFrame::matches_func(std::string const& funcname) const noexcept {
	#ifdef _WIN32
		return str_contains(name               ,funcname);
	#else
		return str_contains(function_identifier,funcname);
	#endif
}
void StackFrame::prettify_strings() {
	#ifdef TINYLEAKCHECK_ENABLED
	memory_tracer->mode.record.push(false);
	{
	#endif
		struct PrettifyReplacement final {
			std::string find, replace;
		} const replacements[] = TINYLEAKCHECK_PRETTIFY_STRS;

		#ifdef _WIN32
			std::string& target_str = name;
			#else
			std::string& target_str = function_identifier;
			#endif
			for (PrettifyReplacement const& replacement : replacements) {
				target_str = str_get_replaced( target_str, replacement.find,replacement.replace );
			}

			#ifdef _WIN32
			std::string shortest_filename = filename;
			for (char const* varname : TINYLEAKCHECK_PRETTIFY_ENVS) {
				char const* var = std::getenv(varname);
				if (var==nullptr) continue;
				std::string repl = str_get_replaced(filename,var,std::string("%")+varname+"%");
				if (repl.length()<shortest_filename.length()) shortest_filename=repl;
			}
			filename = shortest_filename;
		#else
			//TODO: would be nice to have file information . . .
		#endif
	#ifdef TINYLEAKCHECK_ENABLED
	}
	memory_tracer->mode.record.pop();
	#endif
}
void StackFrame::basic_print(FILE* file/*=stderr*/,size_t indent/*=4*/) const noexcept {
	for (size_t i=0;i<indent;++i) fputc(' ',file);
	#ifdef _WIN32
		fprintf(file,"%s!",module.c_str());
		if (name!="") [[likely]] fprintf(file,"%s",name.c_str());
		else                     fprintf(file,"0x%p",return_address);
		if (filename!="") [[likely]] {
			fprintf( file, " at %s(%zu,%zu)\n", filename.c_str(), line,line_offset );
		} else {
			fprintf( file, "\n" );
		}
	#else
		fprintf(file,"%p: %s\n",return_address,function_identifier.c_str());
	#endif
}

#ifdef _WIN32
//Initializing and deinitializing the symbol walking is really slow.  Try to cache this per-process.
struct StackTraceSymConfig final {
	HANDLE const process;
	size_t count;
	explicit StackTraceSymConfig(HANDLE process) : process(process), count(0) {
		//Initialize process symbol handler
		#ifndef NDEBUG
		{ BOOL ret=SymInitialize(process,nullptr,TRUE); TINYLEAKCHECK_ASSERT(ret==TRUE,"Implementation error!"); }
		#else
		           SymInitialize(process,nullptr,TRUE);
		#endif

		//Configure the symbol handler.  See also:
		//	https://docs.microsoft.com/en-us/windows/win32/api/dbghelp/nf-dbghelp-symsetoptions
		SymSetOptions(
			SYMOPT_EXACT_SYMBOLS         | //Do not load non-matching PDB files.
			SYMOPT_FAVOR_COMPRESSED      | //Favor compressed PDB files, of course.
			#if defined TINYLEAKCHECK_AMD64 || defined TINYLEAKCHECK_IA_64
			SYMOPT_INCLUDE_32BIT_MODULES | //In 64-bit mode, allow debugging 32-bit modules, too.
			#endif
			SYMOPT_LOAD_LINES              //Load line numbers
		);
	}
	~StackTraceSymConfig() noexcept {
		#ifndef NDEBUG
			{ BOOL ret=SymCleanup(process); TINYLEAKCHECK_ASSERT(ret==TRUE,"Implementation error!"); }
		#else
			           SymCleanup(process);
		#endif
	}
};
static std::map<HANDLE,StackTraceSymConfig*>* _sym_config = nullptr;
#endif
StackTrace::StackTrace() noexcept :
	thread_id(std::this_thread::get_id())
{
	auto create = [this]() noexcept -> void {
		#ifdef TINYLEAKCHECK_ENABLED
		/*
		Can't do memory tracing here with a stack trace because the implementation of stack tracing
		. . . allocates memory which will then requests a stack trace: an infinite recursion.  At
		the same time, though, we *are* allocating memory, so we still need to record the
		allocations so that the tracer won't be confused when this stack trace is later deallocated.
		*/
		memory_tracer->mode.with_stacktrace.push(false);
		#endif

		#ifdef _WIN32
			//https://www.rioki.org/2017/01/09/windows_stacktrace.html
			//https://gist.github.com/rioki/85ca8295d51a5e0b7c56e5005b0ba8b4

			#if   defined TINYLEAKCHECK_IA_32
				constexpr DWORD const machine_type = IMAGE_FILE_MACHINE_I386;
			#elif defined TINYLEAKCHECK_AMD64
				constexpr DWORD const machine_type = IMAGE_FILE_MACHINE_AMD64;
			#elif defined TINYLEAKCHECK_IA_64
				constexpr DWORD const machine_type = IMAGE_FILE_MACHINE_IA64;
				#error "Not implemented!"
			#else
				#error "Not supported by Windows!"
			#endif
			#if defined TINYLEAKCHECK_IA32 || defined TINYLEAKCHECK_AMD64
				//Note: in this function, we appear to use 64-bit-specific functions, e.g.
				//	`StackWalk64(...)`.  However, these functions do support both 32-bit and 64-bit
				//	code.  See:
				//		https://docs.microsoft.com/en-us/windows/win32/debug/updated-platform-support

				//Get process and thread handles
				HANDLE process = GetCurrentProcess();
				HANDLE thread  = GetCurrentThread ();

				//Preallocate
				//	Symbol struct
				constexpr size_t symbol_backing_num_bytes = sizeof(IMAGEHLP_SYMBOL64) + 255;
				alignas(alignof(IMAGEHLP_SYMBOL64)) uint8_t symbol_buffer[symbol_backing_num_bytes] = {};
				PIMAGEHLP_SYMBOL64 symbol = reinterpret_cast<IMAGEHLP_SYMBOL64*>(symbol_buffer);
				symbol->SizeOfStruct  = sizeof(IMAGEHLP_SYMBOL64);
				symbol->MaxNameLength = symbol_backing_num_bytes - offsetof(IMAGEHLP_SYMBOL64,Name);
				//	Line struct
				IMAGEHLP_LINE64 line; line.SizeOfStruct=sizeof(IMAGEHLP_LINE64);

				//Initialize and configure symbol handler.  This is a global cache (note this
				//	function is protected by mutex), for performance.
				{
					std::map<HANDLE,StackTraceSymConfig*>::iterator iter;
					if (!_sym_config) [[unlikely]] {
						_sym_config = new std::map<HANDLE,StackTraceSymConfig*>;
						ADD_CONFIG: iter=_sym_config->emplace( process, new StackTraceSymConfig(process) ).first;
					} else {
						iter = _sym_config->find(process);
						if (iter==_sym_config->cend()) [[unlikely]] goto ADD_CONFIG;
					}
					++iter->second->count;
				}

				//Read registers
				CONTEXT frame_regs = {};
				frame_regs.ContextFlags = CONTEXT_FULL;
				RtlCaptureContext(&frame_regs);

				//Construct the partial stack "frame" inside the current one.
				STACKFRAME64 next_frame = {};
				#if   defined TINYLEAKCHECK_IA_32
					next_frame.AddrPC.Offset = frame_regs.Eip;
					next_frame.AddrPC.Mode   = AddrModeFlat;
					next_frame.AddrFrame.Offset = frame_regs.Ebp;
					next_frame.AddrFrame.Mode   = AddrModeFlat;
					next_frame.AddrStack.Offset = frame_regs.Esp;
					next_frame.AddrStack.Mode   = AddrModeFlat;
				#elif defined TINYLEAKCHECK_AMD64
					next_frame.AddrPC.Offset = frame_regs.Rip;
					next_frame.AddrPC.Mode   = AddrModeFlat;
					next_frame.AddrFrame.Offset = frame_regs.Rbp;
					next_frame.AddrFrame.Mode   = AddrModeFlat;
					next_frame.AddrStack.Offset = frame_regs.Rsp;
					next_frame.AddrStack.Mode   = AddrModeFlat;
				#else
					#error "Not implemented!"
				#endif

				//Semantically, `StackWalk64(...)` gets the higher stack frame `next_frame` based on
				//	`frame_regs`.
				while (StackWalk64(
					machine_type,
					process, thread,
					&next_frame, &frame_regs,
					nullptr, SymFunctionTableAccess, SymGetModuleBase, nullptr
				)) {
					StackFrame f;

					static_assert(sizeof(void*)==sizeof(DWORD64));
					f.return_address = std::bit_cast<void*>(next_frame.AddrPC.Offset);

					DWORD64 module_base = SymGetModuleBase64( process, next_frame.AddrPC.Offset );
					if (module_base!=0) [[likely]] {
						char filename_buffer[MAX_PATH];
						static_assert(sizeof(HINSTANCE)==sizeof(HMODULE));
						{
							#ifndef NDEBUG
								DWORD ret = GetModuleFileNameA( std::bit_cast<HINSTANCE>(module_base), filename_buffer,MAX_PATH );
								TINYLEAKCHECK_ASSERT(ret>0,"Implementation error!");
							#else
								            GetModuleFileNameA( std::bit_cast<HINSTANCE>(module_base), filename_buffer,MAX_PATH );
							#endif
						}
						std::string filename = filename_buffer;

						size_t i = filename.find_last_of("\\/");
						if (i!=std::string::npos) [[likely]] {
							f.module = filename.substr(i+1);
						} else {
							f.module = filename;
						}
					} else {
						//TODO: `GetLastError()` useful here?
						//f.module = "(unknown module)";
						SetLastError(0);
					}

					//Get symbol name
					if (SymGetSymFromAddr64( process, next_frame.AddrPC.Offset, nullptr, symbol )==TRUE) [[likely]] {
						f.name = symbol->Name;
					} else {
						//TODO: `GetLastError()` useful here?
						//f.name = "(unknown function)";
						SetLastError(0);
					}

					//Get symbol filename, line number, and line offset
					DWORD line_offset = 0;
					if (SymGetLineFromAddr64( process, next_frame.AddrPC.Offset, &line_offset, &line )==TRUE) [[likely]] {
						f.filename    = line.FileName;
						f.line        = line.LineNumber;
						f.line_offset = line_offset;
					} else {
						//TODO: `GetLastError()` useful here?
						f.line        = 0;
						f.line_offset = 0;
						SetLastError(0);
					} 

					//Completed frame; continue to the next one
					frames.push_back(f);
				}
			#endif
		#else
			//See also: https://www.gnu.org/software/libc/manual/html_node/Backtraces.html

			std::vector<void*> return_addresses; return_addresses.resize(10);
			size_t num_frames;
			LOOP:
				num_frames = static_cast<size_t>(backtrace( return_addresses.data(), static_cast<int>(return_addresses.size()) ));
				if (num_frames==return_addresses.size()) {
					return_addresses.resize( return_addresses.size() * 2 );
					goto LOOP;
				}
			return_addresses.resize(num_frames);
			frames.resize(num_frames);

			char** strings = backtrace_symbols(
				return_addresses.data(),
				static_cast<int>(return_addresses.size())
			);
			TINYLEAKCHECK_ASSERT(strings!=nullptr,"Could not get stack trace!");
			struct FreeRAII final {
				char**const ptr;
				constexpr explicit FreeRAII(char** ptr) noexcept : ptr(ptr) {}
				~FreeRAII() { free(ptr); }
			} free_raii(strings);

			for (size_t i=0;i<num_frames;++i) {
				frames[i].return_address = return_addresses[i];
				frames[i].function_identifier = strings[i];
			}

			//TODO: would be nice to have file information . . .
		#endif

		#ifdef TINYLEAKCHECK_ENABLED
		memory_tracer->mode.with_stacktrace.pop();
		#endif
	};
	#ifdef _WIN32
		if (_stacktrace_mutex.ready) {
			//We only allow creating a stack trace from a single thread at a time.  This is because
			//	all "DbgHelp" calls are single-threaded only.
			std::lock_guard lock(_stacktrace_mutex.mutex);

			create();
		} else {
			//However, if the mutex has not been created yet, then we can't use it.  Fortunately,
			//	this situation happens when we're in the static initialization phase, and so we're
			//	single-threaded anyway.
			create();
		}
	#else
		create();
	#endif

	//Don't show `create()` and this constructor; the first frame should be the location in the
	//	caller.
	pop(2);
}
StackTrace::~StackTrace() noexcept {
	#ifdef _WIN32
	std::lock_guard lock(_stacktrace_mutex.mutex);

	TINYLEAKCHECK_ASSERT(_sym_config,"Implementation error!");

	HANDLE process = GetCurrentProcess();
	auto iter = _sym_config->find(process);
	TINYLEAKCHECK_ASSERT(iter!=_sym_config->cend(),"Implementation error!");

	TINYLEAKCHECK_ASSERT(iter->second->count>0,"Implementation error!");
	--iter->second->count;
	if (iter->second->count==0) [[unlikely]] {
		delete iter->second;
		_sym_config->erase(iter);
		if (_sym_config->empty()) [[unlikely]] {
			delete _sym_config; _sym_config=nullptr;
		}
	}
	#endif
}
void StackTrace::basic_print(FILE* file/*=stderr*/,size_t indent/*=4*/) const noexcept {
	for (auto const& frame : frames) {
		frame.basic_print(file,indent);
	}
}

#ifdef TINYLEAKCHECK_ENABLED
MemoryTracer::BlockInfo::BlockInfo( void* ptr, size_t alignment,size_t size, bool with_stacktrace ) noexcept :
	ptr(ptr), alignment(alignment),size(size), thread_id(std::this_thread::get_id())
{
	if (with_stacktrace) [[likely]] {
		trace = new StackTrace;
		/*
		We need to pop four times to get to the call site where the memory was allocated:
			BlockInfo::BlockInfo(...)
			Tracer::record_alloc(...)
			_alloc(...)
			operator new(...) / operator new[](...)
		*/
		trace->pop(4);
	} else {
		//Stack trace was not requested.  This massively improves performance and is actually
		//	necessary e.g. when generating the stack trace itself (we'd get infinite recursion).
		trace = nullptr;
	}
}
void MemoryTracer::BlockInfo::basic_print(FILE* file/*=stderr*/,size_t indent/*=2*/) const noexcept {
	for (size_t i=0;i<indent;++i) fputc(' ',file);
	fputs("Leaked ",file);
	#ifdef _WIN32
	fprintf(file,"0x%p",ptr);
	#else
	fprintf(file,"%p"  ,ptr);
	#endif
	std::stringstream ss; ss<<thread_id; std::string thread_id_str=ss.str();
	fprintf(file," ( align %zu, size %zu, thread %s )",alignment,size,thread_id_str.c_str());

	if (trace!=nullptr) [[likely]] {
		fprintf(file," allocated at:\n");
		trace->basic_print(file,indent+2);
	} else {
		fprintf(file,"\n");
	}
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
inline static void _default_callback_print_block(MemoryTracer const& /*memory_tracer_lcl*/,MemoryTracer::BlockInfo const& block) {
	block.basic_print();
}
inline static void _default_callback_post_alloc (MemoryTracer const& /*memory_tracer_lcl*/,void* /*ptr*/,size_t /*alignment*/,size_t /*size*/) {}
inline static void _default_callback_pre_dealloc(MemoryTracer const& /*memory_tracer_lcl*/,void* /*ptr*/,size_t /*alignment*/                ) {}
[[noreturn]] inline static void _default_callback_leaks_detected(MemoryTracer const& memory_tracer_lcl) {
	fprintf(stderr,"Leaks detected!\n");
	for (auto const& iter : memory_tracer_lcl.blocks) {
		memory_tracer_lcl.callbacks.print_block(memory_tracer_lcl,*iter.second);
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
MemoryTracer::MemoryTracer() {
	callbacks.print_block    = _default_callback_print_block   ;
	callbacks.post_alloc     = _default_callback_post_alloc    ;
	callbacks.pre_dealloc    = _default_callback_pre_dealloc   ;
	callbacks.leaks_detected = _default_callback_leaks_detected;
}
MemoryTracer::~MemoryTracer() {
	if (!blocks.empty()) [[unlikely]] {
		//Prettify strings and process ignores
		std::string const ignore_funcs[] = TINYLEAKCHECK_IGNORE_FUNCS;
		for ( auto iter=blocks.begin(); iter!=blocks.end(); ) {
			BlockInfo* block = iter->second;
			if (block->trace!=nullptr) {
				for (StackFrame& frame : block->trace->frames) {
					frame.prettify_strings();

					for (std::string const& ignore_func : ignore_funcs) {
						if (frame.matches_func(ignore_func)) [[unlikely]] {
							delete block;
							auto iter_old = iter++;
							blocks.erase(iter_old);
							goto NEXT_BLOCK;
						}
					}
				}
			}
			++iter;
			NEXT_BLOCK:;
		}

		//If there are still blocks, then memory leak!
		if (!blocks.empty()) [[unlikely]] {
			callbacks.leaks_detected(*this);

			for (auto const& iter : blocks) {
				delete iter.second;
			}
			//blocks.clear();
		}
	}
}
void MemoryTracer::record_alloc  (void* ptr,size_t alignment,size_t size) {
	std::lock_guard<std::recursive_mutex> lock_raii(_memory_tracer_mutex);

	if (mode.record.peek()) {
		mode.record.push(false);

		blocks.emplace( ptr, new BlockInfo(ptr,alignment,size,mode.with_stacktrace.peek()) );

		callbacks.post_alloc(*this,ptr,alignment,size);

		mode.record.pop();
	}
}
void MemoryTracer::record_dealloc(void* ptr,size_t alignment            ) {
	if (ptr==nullptr) return;

	std::lock_guard<std::recursive_mutex> lock_raii(_memory_tracer_mutex);

	if (mode.record.peek()) {
		mode.record.push(false);

		callbacks.pre_dealloc(*this,ptr,alignment);

		auto iter = blocks.find(ptr);
		TINYLEAKCHECK_ASSERT(iter!=blocks.cend(),"Deleting an invalid pointer 0x%p!",ptr);
		delete iter->second;
		blocks.erase(iter);

		mode.record.pop();
	}
}

/*
When a `MemoryTracer` exists, it can record allocations, and when it's deleted it can report the
allocations that weren't freed as memory leaks.  So when should the instance `memory_tracer` be
created and destroyed?

We can't tie it closely to the `operator new(...)` / `operator delete(...)` functions, because the
whole point is that these might be mismatched.  Another alternative is to make the user allocate /
deallocate it.  This is inconvenient, and of course the user is trying to debug mismatched
allocation / deallocation in the first place.  The right solution is to make it a namespace-scope
variable.

Note, however, that this solution (along with the previous, as it happens) has a subtle problem:
allocation can happen during normal runtime, but get freed by the standard library after the fact,
causing a false positive.  A (real) example is something like:
	(1) Static construction of `memory_tracer`.
	(2) Beginning of `main(...)`.
	(3) User code does file IO, standard library allocates memory.
	(4) End of `main(...)`.
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
inline static void* _alloc  (size_t alignment,size_t size) {
	void* result = aligned_malloc( alignment, size );
	if (_ready) [[likely]] memory_tracer->record_alloc(result,alignment,size);
	return result;
}
inline static void  _dealloc(size_t alignment,void* ptr  ) {
	if (_ready) [[likely]] memory_tracer->record_dealloc(ptr,alignment);
	aligned_free(ptr);
}
struct _EnsureMemoryTracer final {
	_EnsureMemoryTracer() {
		memory_tracer = new MemoryTracer;
		_ready = true;
	}
	~_EnsureMemoryTracer() noexcept {
		_ready = false;
		delete memory_tracer; memory_tracer=nullptr;
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

[[nodiscard]] void* operator new( std::size_t size                             ) {
	return TinyLeakCheck::_alloc(__STDCPP_DEFAULT_NEW_ALIGNMENT__,size);
}
[[nodiscard]] void* operator new( std::size_t size, std::align_val_t alignment ) {
	return TinyLeakCheck::_alloc(static_cast<size_t>(alignment)  ,size);
}

void operator delete( void* ptr                             ) noexcept {
	TinyLeakCheck::_dealloc(__STDCPP_DEFAULT_NEW_ALIGNMENT__,ptr);
}
void operator delete( void* ptr, std::align_val_t alignment ) noexcept {
	TinyLeakCheck::_dealloc(static_cast<size_t>(alignment)  ,ptr);
}

#endif

#if defined __GNUC__ && !defined __clang__

#pragma GCC diagnostic pop

#endif
