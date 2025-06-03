#include <tinyleakcheck/tinyleakcheck.hpp>



static void function_C()
{
	TinyLeakCheck::StackTrace stack_trace;
	stack_trace.basic_print(stdout,0);
}

static void function_B()
{
	function_C();
}

static void function_A()
{
	function_B();
}

int main( int /*argc*/, char* /*argv*/[] )
{
	TinyLeakCheck::prevent_linker_elison();

	function_A();

	return 0;
}
