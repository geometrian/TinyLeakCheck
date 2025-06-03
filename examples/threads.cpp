#include <chrono>
#include <thread>
#include <tinyleakcheck/tinyleakcheck.hpp>



static void main_thread1()
{
	//Leaking 5 * sizeof(int16_t) = 10 bytes from child thread 1.
	for ( unsigned k=0u; k<5u; ++k )
	{
		printf("Child thread 1 is leaking . . .\n");
		new int16_t;
		std::this_thread::sleep_for( std::chrono::milliseconds(10) );
	}
}
static void main_thread2()
{
	//Leaking 5 * sizeof(int32_t) = 20 bytes from child thread 2.
	for ( unsigned k=0u; k<5u; ++k )
	{
		printf("Child thread 2 is leaking . . .\n");
		new int32_t;
		std::this_thread::sleep_for( std::chrono::milliseconds(10) );
	}
}

int main( int /*argc*/, char* /*argv*/[] )
{
	TinyLeakCheck::prevent_linker_elison();

	//Leaking 1 * sizeof(int8_t) = 1 byte from main thread.
	printf("Main thread is leaking . . .\n");
	new int8_t;

	std::thread thread1(main_thread1);
	std::thread thread2(main_thread2);
	thread1.join();
	thread2.join();

	return 0;
}
