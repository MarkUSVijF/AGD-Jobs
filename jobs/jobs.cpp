#include "pch.h"

/*
 * Please add your names and UIDs in the form: Name <uid>, ...
 */

// Remotery is a easy to use profiler that can help you with seeing execution order and measuring data for your implementation
// Once initialized, you can use it by going into the "vis" folder of the project and opening "vis/index.html" in a browser (double-click)
// It emits 3 warnings at the moment, does can be ignored and will not count as warnings emitted by your code! ;)
// 
// Github: https://github.com/Celtoys/Remotery
#include "Remotery/Remotery.h"

using namespace std;

#define MAKE_UPDATE_FUNC(NAME, DURATION) \
	void Update##NAME() { \
		rmt_ScopedCPUSample(NAME, 0); \
		auto start = chrono::high_resolution_clock::now(); \
		decltype(start) end; \
		do { \
			end = chrono::high_resolution_clock::now(); \
		} while (chrono::duration_cast<chrono::microseconds>(end - start).count() < (DURATION)); \
	} \

// You can create other functions for testing purposes but those here need to run in your job system
// The dependencies are noted on the right side of the functions, the implementation should be able to set them up so they are not violated and run in that order!
MAKE_UPDATE_FUNC(Input, 200) // no dependencies
	MAKE_UPDATE_FUNC(Physics, 1000) // depends on Input
		MAKE_UPDATE_FUNC(Collision, 1200) // depends on Physics
			MAKE_UPDATE_FUNC(Animation, 600) // depends on Collision
			MAKE_UPDATE_FUNC(Particles, 800) // depends on Collision
		MAKE_UPDATE_FUNC(GameElements, 2400) // depends on Physics
				MAKE_UPDATE_FUNC(Rendering, 2000) // depends on Animation, Particles, GameElements
MAKE_UPDATE_FUNC(Sound, 1000) // no dependencies

void UpdateSerial()
{
	rmt_ScopedCPUSample(UpdateSerial, 0);
	UpdateInput();
	UpdatePhysics();
	UpdateCollision();
	UpdateAnimation();
	UpdateParticles();
	UpdateGameElements();
	UpdateRendering();
	UpdateSound();
}

// In `UpdateParallel` you should use your jobsystem to distribute the tasks
void UpdateParallel()
{
	rmt_ScopedCPUSample(UpdateParallel, 0);
	UpdateInput();
	UpdatePhysics();
	UpdateCollision();
	UpdateAnimation();
	UpdateParticles();
	UpdateGameElements();
	UpdateRendering();
	UpdateSound();
}

int main()
{
	/*
	 * This initializes remotery, you are not forced to use it (although it's helpful)
	 * but please also don't remove it from here then. Because if you don't use it, I
	 * will most likely do so, to track how your jobs are issued and if the dependencies run
	 * properly
	 */
	Remotery* rmt;
	rmt_CreateGlobalInstance(&rmt);

	atomic<bool> isRunning = true;

	thread serial([&isRunning]()
	{
		while (isRunning)
			UpdateSerial();
	});

	thread parallel([&isRunning]()
	{
		while (isRunning)
			UpdateParallel();
	});

	cout << "Type anything to quit...\n";
	char c;
	cin >> c;
	cout << "Quitting...\n";
	isRunning = false;

	serial.join();
	parallel.join();

	rmt_DestroyGlobalInstance(rmt);
}

