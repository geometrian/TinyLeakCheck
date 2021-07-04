#include <tinyleakcheck/tinyleakcheck.hpp>

inline static void function_C() {
	TinyLeakCheck::StackTrace stack_trace;
	stack_trace.basic_print(stdout,0);
}

inline static void function_B() {
	function_C();
}

inline static void function_A() {
	function_B();
}

int main(int,char*[]) {
	TinyLeakCheck::prevent_linker_elison();

	function_A();

	return 0;
}
