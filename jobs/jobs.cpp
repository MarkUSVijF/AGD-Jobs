#include "pch.h"
#include <functional>
#include <queue>
#include <unordered_map>
#include <vector>
#include <mutex>  // For std::unique_lock
#include <shared_mutex>
#include <thread>

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
//#define IGNORE_DEBUG_OUT

std::mutex mutex_console;
#ifndef IGNORE_DEBUG_OUT
std::vector<std::string> categories = { "temp", "timing", "Warning" };
#else
std::vector<string> categories = {};
#endif // !1


#define DEBUG_OUT(MSG, CATEGORY) { \
if(std::find(std::begin(categories), std::end(categories), CATEGORY) != std::end(categories)) { \
        mutex_console.lock(); \
        cout << MSG << "\n"; \
        mutex_console.unlock(); \
    } \
}


using namespace std;

#ifndef IGNORE_REMOTERY_SHIT
#define REMOTERY_SCOPE(NAME) {rmt_ScopedCPUSample(NAME, 0);}
#else
#define REMOTERY_SCOPE(NAME) {}
#endif // !1

#define MAKE_UPDATE_FUNC(NAME, DURATION) \
	void Update##NAME() { \
		REMOTERY_SCOPE(NAME) \
		auto start = chrono::high_resolution_clock::now(); \
		decltype(start) end; \
		do { \
			end = chrono::high_resolution_clock::now(); \
		} while (chrono::duration_cast<chrono::microseconds>(end - start).count() < (DURATION)); \
	} \

#define GENERAL_SLOWDOWN 1
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
// 'max' 9200, min 5600 // wir haben noch 2 timing jobs drin

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





void sleep(int microsec) {
#ifndef IGNORE_REMOTERY_SHIT
	rmt_ScopedCPUSample(sleep, 0);
#endif // IGNORE_REMOTERY_SHIT
	std::this_thread::sleep_for(chrono::microseconds(microsec));
}







#define MAX_DEPENDENCIES 8 // do we still use this?
#define JOB_RESERVE 200 // max of UINT32_MAX-1
#define THREAD_COUNT 4

#define MULTITHREADED

struct job {
	//uint32_t id; // job_id
	std::function<void(void)> functionToDo;
	std::vector<uint32_t> dependencies; // job_ids
	bool ísReady = false;
	bool finished = false;
};

// our scheduler stuff:
std::shared_mutex mutex_fullLock;

std::shared_mutex mutex_allJobs;
std::unordered_map<uint32_t, job> allJobs; // jobHandle -> struct (with own dipendencies)

std::shared_mutex mutex_scheduledJobs_ready;
std::queue<uint32_t> scheduledJobs_ready; // camel_case with snakeCase is best case !!

std::shared_mutex mutex_scheduledJobs_waiting;
std::unordered_map<uint32_t, std::vector<uint32_t>> scheduledJobs_waiting; // job -> jobs waiting on it

std::shared_mutex mutex_topJobID;
uint32_t topJobID = 1;



// an ID of Zero means BUG
uint32_t CreateJob(std::function<void(void)> functionToDo, std::vector<uint32_t> &dependencies) {

	mutex_fullLock.lock(); // one thread only

	mutex_allJobs.lock();// _shared();
	if (allJobs.size() >= JOB_RESERVE) { // (Rlock)
		mutex_allJobs.unlock();// _shared();

		mutex_fullLock.unlock();
		return 0; // no space available
	}
	//cout << allJobs.size()+1 << "   " << JOB_RESERVE << "\n";
	mutex_allJobs.unlock();// _shared();

	uint32_t id;
	{ // Wlock[topJobID]
		mutex_topJobID.lock();
		/*
		while (!mutex_topJobID.try_lock()) {
			yield(); // re-queue thread
		}
		*/

		id = topJobID; // Rlock
		/* in the very unlikely case, that a job takes longer than it takes to finish UINT32_MAX jobs */
		while (allJobs.find(id) != allJobs.end()) { // that is VERY ugly - its a curse !!! - plz fix #TODO // (Rlock)
			if (id == UINT32_MAX) {
				id = 0;
			}
			id++;
		} // this is fine */

		if (id != UINT32_MAX) {
			topJobID = id + 1;// Wlock
		}
		else {
			topJobID = 1;// Wlock
		}

		mutex_topJobID.unlock();
	}


	std::vector<uint32_t> dep;
	dep.reserve(8);
	// add to all jobs

	mutex_allJobs.lock();
	for (uint32_t depI : dependencies) {
		if (allJobs.find(depI) != allJobs.end()) {
			dep.push_back(depI);
		}
	}
	dep.shrink_to_fit();

	DEBUG_OUT(id << " <new "<< dep.size(),"debug");
	

	allJobs[id] = job{ functionToDo,dep }; // Wlock


	if (dep.size() != 0) { // highly likely we have a dependency

		mutex_scheduledJobs_waiting.lock();

		for (uint32_t dependantID : dep) {
			// scheduledJobs_waiting[dependantID] creates a new element if needed!!!
			if (allJobs.find(dependantID) != allJobs.end())
				scheduledJobs_waiting[dependantID].push_back(id); // Wlock
		}

		mutex_scheduledJobs_waiting.unlock();
		mutex_allJobs.unlock();
	}
	else {
		mutex_allJobs.unlock();
		mutex_scheduledJobs_ready.lock();

		scheduledJobs_ready.push(id); // Wlock

		mutex_scheduledJobs_ready.unlock();
	}

	mutex_fullLock.unlock();
	return id;
}

// worker thread stuff:

void FinishJob(uint32_t jobId); // this we need - exactly here - it purrrfect
void WorkerMainLoop() {
	uint32_t jobID;
	job j;
#ifndef MULTITHREADED
	while (true) { // #TODO - get a life
#endif // MULTITHREADED

		//std::this_thread::sleep_for(chrono::microseconds(10000));
		/*
		if (!mutex_scheduledJobs_ready.try_lock()) {
			sleep(10000);
			//cout << std::this_thread::get_id() << " !!!!! "<< (allJobs.size()>= JOB_RESERVE)<< "|" << scheduledJobs_ready.size() << "\n";
			return;
		}
		*/
		mutex_scheduledJobs_ready.lock(); // Wlock
		//cout << "jobs ready: " << scheduledJobs_ready.size() << "\n";
		if (!scheduledJobs_ready.empty()) { // (Rlock)

			// get a job
			{
				//mutex_scheduledJobs_ready.lock(); // moved before if

				jobID = scheduledJobs_ready.front(); // Rlock
				scheduledJobs_ready.pop(); // Wlock

				mutex_scheduledJobs_ready.unlock(); // Rlock
				//DEBUG_OUT(std::this_thread::get_id() << " - " << jobID);
			}
			{
				mutex_allJobs.lock_shared();
				j = allJobs[jobID]; // Rlock
				//allJobs.erase(jobID); // Wlock
				mutex_allJobs.unlock_shared();
			}

			//cout << std::this_thread::get_id() << "_2\n";
			// do job
			j.functionToDo();
			j.finished = true;

			DEBUG_OUT(jobID << " fin1", "debug");

			//cout << std::this_thread::get_id() << "_3\n";
			// tell that you finished
			FinishJob(jobID);
			DEBUG_OUT(jobID << " fin2", "debug");

			//cout << std::this_thread::get_id() << "_4\n";
			// now -> repeat
		}
		else {
			// no job awailable
			mutex_scheduledJobs_ready.unlock();

#ifdef MULTITHREADED

			//std::this_thread::sleep_for(chrono::microseconds(1000000)); 
			std::this_thread::yield();// yield seems to be broken on my PC
#else
			break;
#endif // MULTITHREADED
		}

#ifndef MULTITHREADED
	}
#endif // MULTITHREADED
}

void FinishJob(uint32_t jobID) {

	mutex_fullLock.lock(); // one thread only
	std::vector<uint32_t> waitingOnMe;

	mutex_scheduledJobs_waiting.lock();// _shared();
	if (scheduledJobs_waiting.find(jobID) != scheduledJobs_waiting.end()) {
		waitingOnMe = std::vector<uint32_t>(scheduledJobs_waiting[jobID]); // Rlock
		mutex_scheduledJobs_waiting.unlock();// _shared();
	}
	else {

		DEBUG_OUT(jobID << "|1" << "-:-", "debug");
		mutex_allJobs.lock();
		//cout << allJobs.size() << " 0> ";
		allJobs.erase(jobID); // Wlock - muss existieren
		//cout << allJobs.size() << "\n";
		mutex_allJobs.unlock();
		DEBUG_OUT(jobID << "|2" << "-:-", "debug");
		mutex_scheduledJobs_waiting.unlock();// _shared();

		mutex_fullLock.unlock();
		return;
	}


	for (uint32_t id : waitingOnMe) {
		uint32_t hasDependencies = 0;

		/*
		while (!mutex_allJobs.try_lock()) {
			//cout << jobID << " Hello!\n";
			sleep(100);
		}*/
		mutex_allJobs.lock();// _shared();

		if (allJobs.find(id) == allJobs.end()) {
			continue;
		}
		for (uint8_t i = 0; i < allJobs[id].dependencies.size(); i++) { // Rlock
			if (allJobs[id].dependencies[i] == jobID) { // Rlock
				allJobs[id].dependencies[i] = 0; // Rlock
			}
			hasDependencies |= allJobs[id].dependencies[i]; // Rlock
		}
		DEBUG_OUT(jobID << "|" << id << ":" << hasDependencies, "debug");
		mutex_allJobs.unlock();// _shared();

		if (!hasDependencies) {
			mutex_scheduledJobs_ready.lock();
			scheduledJobs_ready.push(id); // Wlock
			mutex_scheduledJobs_ready.unlock();

			mutex_allJobs.lock();
			if (allJobs.find(id) != allJobs.end())
				allJobs[id].ísReady = true;
			else {
				DEBUG_OUT("NOOOOOOOOOOOOOO", "debug");
			}
			mutex_allJobs.unlock();
		}
	}

	{
		mutex_scheduledJobs_waiting.lock();
		scheduledJobs_waiting.erase(jobID); // Wlock
		mutex_scheduledJobs_waiting.unlock();
	}

	{ 
		mutex_allJobs.lock();
		//cout << allJobs.size() << " 1> ";
		allJobs.erase(jobID); // Wlock
		//cout << allJobs.size() << "\n";
		mutex_allJobs.unlock();
	}
	mutex_fullLock.unlock();
}

std::mutex mutex_timing;
long long duration_floatingAverage = 0;
long long frame_floatingAverage = 0;
chrono::steady_clock::time_point frame_last;

// In `UpdateParallel` you should use your jobsystem to distribute the tasks
void UpdateParallel(atomic<bool> &isRunning)
{
	//cout << "##";
#ifndef IGNORE_REMOTERY_SHIT
	rmt_ScopedCPUSample(UpdateParallel, 0);
#endif // IGNORE_REMOTERY_SHIT

	/*
	uint32_t inputJobID, physicsJobID, collisionJobID, animationJobID, particleJobID, gameElementJobID, renderingJobID, soundJobID;
	inputJobID = CreateJob(UpdateInput, {});
	physicsJobID = CreateJob(UpdatePhysics, { inputJobID });
	collisionJobID = CreateJob(UpdateCollision, { physicsJobID });
	animationJobID = CreateJob(UpdateAnimation, { collisionJobID });
	particleJobID = CreateJob(UpdateParticles, { collisionJobID });
	gameElementJobID = CreateJob(UpdateGameElements, { physicsJobID });
	renderingJobID = CreateJob(UpdateRendering, { animationJobID ,particleJobID ,gameElementJobID });
	soundJobID = CreateJob(UpdateSound, {});
	*/

	chrono::steady_clock::time_point* timeStart = new chrono::steady_clock::time_point;

	std::function<void(void)> jobfunctions[10] = {
		[timeStart] {
		mutex_timing.lock();
		*timeStart = chrono::high_resolution_clock::now();
		mutex_timing.unlock();
		},
		UpdateInput ,UpdatePhysics ,UpdateCollision ,UpdateAnimation, UpdateParticles, UpdateGameElements, UpdateRendering, UpdateSound,
		[timeStart] { // how long this 'frame' took - frames can be intersecting if added faster than finished

		mutex_timing.lock();
		if (frame_floatingAverage!=0) {
			long long frame = (chrono::duration_cast<chrono::microseconds>(chrono::high_resolution_clock::now() - frame_last).count());
			frame_floatingAverage /= 10;
			frame_floatingAverage *= 9;
			frame_floatingAverage += (frame / 10);
			frame_last = chrono::high_resolution_clock::now();
			DEBUG_OUT(frame_floatingAverage << " | " << frame, "timing");
		}
		else {
			frame_last = chrono::high_resolution_clock::now();
			frame_floatingAverage = 1;
		}

		long long duration = (chrono::duration_cast<chrono::microseconds>(chrono::high_resolution_clock::now() - *timeStart).count());
		duration_floatingAverage /= 10;
		duration_floatingAverage *= 9;
		duration_floatingAverage += (duration / 10);

		DEBUG_OUT(duration_floatingAverage << " | " << duration, "timing");
		mutex_timing.unlock();

		mutex_allJobs.lock();
		DEBUG_OUT(allJobs.size(), "temp");
		mutex_allJobs.unlock();

		delete timeStart; } };
	std::vector<uint32_t> jobDependencies[10] = { {}, {0}, {1}, {2}, {3}, {3}, {2}, {4,5,6}, {0}, {1,7,8} };
	uint32_t jobIDs[10];

	for (int i = 0; i < 10; i++) {
		std::vector<uint32_t> dependencies;
		for (int pos : jobDependencies[i])
		{
			dependencies.push_back(jobIDs[pos]);
		}
		while (isRunning) {
			jobIDs[i] = CreateJob(jobfunctions[i], dependencies);
			if (jobIDs[i] != 0) {
				break;
			}
			DEBUG_OUT( "allJobs full!!! " << scheduledJobs_ready.size()<<","<< scheduledJobs_waiting.size() , "Warning");
			std::this_thread::yield();
			//sleep(100000);
		}
	}

#ifndef MULTITHREADED
	WorkerMainLoop();
#endif // MULTITHREADED

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

#ifndef IGNORE_REMOTERY_SHIT
	Remotery* rmt;
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
		while (isRunning) {
			UpdateParallel(isRunning);
			sleep(1);//5000, 10
			//std::this_thread::sleep_for(chrono::microseconds(0));
		}

	});

	thread* workers[THREAD_COUNT];
#ifdef MULTITHREADED
	for (int i = 0; i < THREAD_COUNT; i++) {
		workers[i] = new thread([&isRunning]()
		{
			while (isRunning)
				WorkerMainLoop();
		});
	}
#endif // MULTITHREADED

	//cout << allJobs.size();
	cout << "Type anything to quit...\n";
	char c;
	cin >> c;
	cout << "Quitting...\n";
	isRunning = false;

	serial.join();
	cout << "serial\n";
	parallel.join();
	cout << "parallel\n";

	for (int i = 0; i < THREAD_COUNT; i++) {
		workers[i]->join();
		cout << "workers"<<i<<"\n";
	}

	cout << "Finished!!!\n";
	cout << "Tried "<< topJobID << " Jobs.\n";

#ifndef IGNORE_REMOTERY_SHIT
	rmt_DestroyGlobalInstance(rmt);
#endif // !IGNORE_REMOTERY_SHIT
}