//Either one (or both) is sufficient.
#include <tinyleakcheck/tinyleakcheck.hpp>
//#include <tinyleakcheck/tiniestleakcheck.hpp>

#include <string>
#ifdef _WIN32
	#include <Windows.h>
#endif



//Static leak!  Not detectable on some C++-spec-violating platforms.
//	BTW it is bad practice to dynamically allocate static memory.  Try to avoid.
std::string str;

static void function_C()
{
	//Memory leak!  This pointer should have been captured and `delete`d.
	new int;
}

static void function_B()
{
	function_C();

	//Memory leak!  This pointer should have been captured and `delete`d.
	new char;
}

static void function_A()
{
	function_B();
}

int main( int /*argc*/, char* /*argv*/[] )
{
	TinyLeakCheck::prevent_linker_elison();

	#ifdef _WIN32
		/*
		This all just makes the default console a bit bigger, as it's rather small on Windows---the
		better to allow the glorious detail of the memory leak trace to be properly appreciated :3
		*/
		void* handle = GetStdHandle(STD_OUTPUT_HANDLE);
		size_t res[2]={ 185,40 }, scrolls=200;
		COORD      arg_buffer_size = {       (SHORT)(res[0]  ), (SHORT)(res[1]+scrolls) };
		SMALL_RECT arg_window_rect = { 0, 0, (SHORT)(res[0]-1), (SHORT)(res[1]-1      ) };
		SetConsoleScreenBufferSize( handle, arg_buffer_size );
		SetConsoleWindowInfo( handle, TRUE, &arg_window_rect );
	#endif

	function_A();

	return 0;
}
