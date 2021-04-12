#include "pch.h"
#include <functional>
#include <queue>
#include <unordered_map>
#include <vector>

/*
 * Please add your names and UIDs in the form: Name <uid>, ...
 */

// Remotery is a easy to use profiler that can help you with seeing execution order and measuring data for your implementation
// Once initialized, you can use it by going into the "vis" folder of the project and opening "vis/index.html" in a browser (double-click)
// It emits 3 warnings at the moment, does can be ignored and will not count as warnings emitted by your code! ;)
// 
// Github: https://github.com/Celtoys/Remotery
#include "Remotery/Remotery.h"
#define IGNORE_REMOTERY_SHIT

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

#define GENERAL_SLOWDOWN 25
// You can create other functions for testing purposes but those here need to run in your job system
// The dependencies are noted on the right side of the functions, the implementation should be able to set them up so they are not violated and run in that order!
MAKE_UPDATE_FUNC(Input, 200* GENERAL_SLOWDOWN) // no dependencies
	MAKE_UPDATE_FUNC(Physics, 1000 * GENERAL_SLOWDOWN) // depends on Input
		MAKE_UPDATE_FUNC(Collision, 1200 * GENERAL_SLOWDOWN) // depends on Physics
			MAKE_UPDATE_FUNC(Animation, 600 * GENERAL_SLOWDOWN) // depends on Collision
			MAKE_UPDATE_FUNC(Particles, 800 * GENERAL_SLOWDOWN) // depends on Collision
		MAKE_UPDATE_FUNC(GameElements, 2400 * GENERAL_SLOWDOWN) // depends on Physics
				MAKE_UPDATE_FUNC(Rendering, 2000 * GENERAL_SLOWDOWN) // depends on Animation, Particles, GameElements
MAKE_UPDATE_FUNC(Sound, 1000 * GENERAL_SLOWDOWN) // no dependencies

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













#define MAX_DEPENDENCIES 8 // do we still use this?
#define JOB_RESERVE 200 // max of UINT32_MAX-1

struct job {
	//uint32_t id; // job_id
	std::function<void(void)> functionToDo;
	std::vector<uint32_t> dependencies; // job_ids
};

// our scheduler stuff:
std::unordered_map<uint32_t, job> allJobs; // jobHandle -> struct (with own dipendencies)
std::queue<uint32_t> scheduledJobs_ready; // camel_case with snakeCase is best case !!
std::unordered_map<uint32_t, std::vector<uint32_t>> scheduledJobs_waiting; // job -> jobs waiting on it
uint32_t topJobID = 1;

// an ID of Zero means BUG
uint32_t CreateJob(std::function<void(void)> functionToDo, std::vector<uint32_t> dependencies) {

	if (allJobs.size() == allJobs.max_size()) {
		return 0; // no space available
	}

	uint32_t id = topJobID;
	/* in the very unlikely case, that a job takes longer than it takes to finish UINT32_MAX jobs */
	while (allJobs.find(id) != allJobs.end()) { // that is VERY ugly - its a curse !!! - plz fix #TODO
		if (id == UINT32_MAX) {
			id = 0;
		}
		id++;
	} // this is fine */

	if (id != UINT32_MAX) {
		topJobID = id + 1;
	}
	else {
		topJobID = 1;
	}

	// add to all jobs

	allJobs[id]= job{ functionToDo,dependencies };

	if (dependencies.size() != 0) { // highly likely we have a dependency

		for (uint32_t dependantID : dependencies) {
			scheduledJobs_waiting[dependantID].push_back(id);
		}
	}
	else {
		scheduledJobs_ready.push(id);
	}

	return id;
}

// worker thread stuff:

void FinishJob(uint32_t jobId); // this we need - exactly here - it purrrfect
void WorkerMainLoop() {
	while (true) { // #TODO - get a life

		if (!scheduledJobs_ready.empty()) {

			// get a job
			uint32_t jobID = scheduledJobs_ready.front();
			scheduledJobs_ready.pop();
			job j = allJobs[jobID];

			// do job
			j.functionToDo();

			// tell that you finished
			FinishJob(jobID);

			// repeat
			continue;
		}
		else {
			// yield #TODO
			break;
		}
	}
}

void FinishJob(uint32_t jobId) {
	for (uint32_t id : scheduledJobs_waiting[jobId]) {
		uint32_t hasDependencies = 0;
		for (uint8_t i = 0; i < allJobs[id].dependencies.size(); i++) {
			if (allJobs[id].dependencies[i] == jobId) {
				allJobs[id].dependencies[i] = 0;
			}
			hasDependencies |= allJobs[id].dependencies[i];
		}
		if (!hasDependencies) {
			scheduledJobs_ready.push(id);
		}
	}
	scheduledJobs_waiting.erase(jobId);
	allJobs.erase(jobId);
}

// In `UpdateParallel` you should use your jobsystem to distribute the tasks
void UpdateParallel()
{
	//cout << "##";
#ifndef IGNORE_REMOTERY_SHIT
	rmt_ScopedCPUSample(UpdateParallel, 0);
#endif // IGNORE_REMOTERY_SHIT

	//cout << "##\n";
	//cout << topJobID;
	//cout << "\n";
	uint32_t inputJobID = CreateJob(UpdateInput, {});
	//cout << inputJobID;
	//cout << ",";
	uint32_t physicsJobID = CreateJob(UpdatePhysics, { inputJobID });
	//cout << physicsJobID;
	//cout << ",";
	uint32_t collisionJobID = CreateJob(UpdateCollision, { physicsJobID });
	//cout << collisionJobID;
	//cout << ",";
	uint32_t animationJobID = CreateJob(UpdateAnimation, { collisionJobID });
	//cout << animationJobID;
	//cout << ",";
	uint32_t particleJobID = CreateJob(UpdateParticles, { collisionJobID });
	//cout << particleJobID;
	//cout << ",";
	uint32_t gameElementJobID = CreateJob(UpdateGameElements, { physicsJobID });
	//cout << gameElementJobID;
	//cout << ",";
	uint32_t renderingJobID = CreateJob(UpdateRendering, { animationJobID ,particleJobID ,gameElementJobID });
	//cout << renderingJobID;
	//cout << ",";
	uint32_t soundJobID = CreateJob(UpdateSound, {});
	//cout << soundJobID;
	//cout << ",";

	//cout << "\n";

	WorkerMainLoop();

	//cout << topJobID;
	//cout << "\n";
}

int main()
{
	// init stuff
	allJobs.reserve(JOB_RESERVE);
	/*
	 * This initializes remotery, you are not forced to use it (although it's helpful)
	 * but please also don't remove it from here then. Because if you don't use it, I
	 * will most likely do so, to track how your jobs are issued and if the dependencies run
	 * properly
	 */
	Remotery* rmt;

#ifndef IGNORE_REMOTERY_SHIT
	rmt_CreateGlobalInstance(&rmt);
#endif // !IGNORE_REMOTERY_SHIT


	atomic<bool> isRunning = true;


	thread serial([&isRunning]()
	{
		while (isRunning)
			UpdateSerial();
	});

	// our scheduler
	/*
	thread scheduler([&isRunning]()
	{
		
		while (isRunning) {

		}
	});
	*/

	thread parallel([&isRunning]()
	{
		while (isRunning)
			UpdateParallel();
	});

	cout << allJobs.size();
	cout << "Type anything to quit...\n";
	char c;
	cin >> c;
	cout << "Quitting...\n";
	isRunning = false;

	serial.join();
	parallel.join();

#ifndef IGNORE_REMOTERY_SHIT
	rmt_DestroyGlobalInstance(rmt);
#endif // !IGNORE_REMOTERY_SHIT
}

/* TODO
- thread this shit & synch it




- delete this
*/