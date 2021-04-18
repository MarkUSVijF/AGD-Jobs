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

//#####################################################################################################################

#define IGNORE_REMOTERY_SHIT

#define MAX_DEPENDENCIES 8 // do we still use this?
#define JOB_RESERVE 200 // max of UINT32_MAX-1
#define THREAD_COUNT 4

#define MULTITHREADED

#define DEBUG_CATEGORIES { "temp", "timing", "_Warning" }
//#define RUN_TIMERS


//#####################################################################################################################


std::mutex mutex_console;

#ifdef DEBUG_CATEGORIES
#define DEBUG_OUT(MSG, CATEGORY) { \
if(std::find(std::begin(DEBUG_CATEGORIES), std::end(DEBUG_CATEGORIES), CATEGORY) != std::end(DEBUG_CATEGORIES)) { \
        mutex_console.lock(); \
        cout << MSG << "\n"; \
        mutex_console.unlock(); \
    } \
}
#else // !DEBUG_CATEGORIES
#define DEBUG_OUT(MSG, CATEGORY) {}
#endif // DEBUG_CATEGORIES



using namespace std;

#ifdef IGNORE_REMOTERY_SHIT
#define REMOTERY_SCOPE(NAME) {}
#else // !IGNORE_REMOTERY_SHIT
#define REMOTERY_SCOPE(NAME) {rmt_ScopedCPUSample(NAME, 0);}
#endif // IGNORE_REMOTERY_SHIT

//#####################################################################################################################

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
MAKE_UPDATE_FUNC(Input, 200 * GENERAL_SLOWDOWN) // no dependencies
MAKE_UPDATE_FUNC(Physics, 1000 * GENERAL_SLOWDOWN) // depends on Input(0)
MAKE_UPDATE_FUNC(Collision, 1200 * GENERAL_SLOWDOWN) // depends on Physics(1)
MAKE_UPDATE_FUNC(Animation, 600 * GENERAL_SLOWDOWN) // depends on Collision(2)
MAKE_UPDATE_FUNC(Particles, 800 * GENERAL_SLOWDOWN) // depends on Collision(2)
MAKE_UPDATE_FUNC(GameElements, 2400 * GENERAL_SLOWDOWN) // depends on Physics(1)
MAKE_UPDATE_FUNC(Rendering, 2000 * GENERAL_SLOWDOWN) // depends on Animation(3), Particles(4), GameElements(5)
MAKE_UPDATE_FUNC(Sound, 1000 * GENERAL_SLOWDOWN) // no dependencies
// 'max' 9200, min 5600 // wir haben noch 2 timing jobs drin

//#####################################################################################################################

void sleep(int microsec) {
#ifndef IGNORE_REMOTERY_SHIT
	rmt_ScopedCPUSample(sleep, 0);
#endif // !IGNORE_REMOTERY_SHIT
	std::this_thread::sleep_for(chrono::microseconds(microsec));
}

//#####################################################################################################################

namespace Scheduler {
	struct job {
		//uint32_t id; // job_id
		std::function<void(void)> functionToDo;
		std::vector<uint32_t> dependencies; // job_ids
		//bool ísReady = false;
		//bool finished = false;
	};
	// our scheduler stuff:
	std::shared_mutex mutex_fullLock;

	std::shared_mutex mutex_topJobID;
	uint32_t topJobID = 1;

	std::shared_mutex mutex_allJobs;
	std::unordered_map<uint32_t, job> allJobs; // jobHandle -> struct (with own dipendencies)

	std::shared_mutex mutex_jobsReady;
	std::queue<uint32_t> jobsReady; // camel_case with snakeCase is best case !!

	std::shared_mutex mutex_jobsWaiting;
	std::unordered_map<uint32_t, std::vector<uint32_t>> jobsWaiting; // job -> jobs waiting on it


	uint32_t CreateJob(std::function<void(void)> functionToDo, std::vector<uint32_t>& dependencies) {

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

		DEBUG_OUT(id << " <new " << dep.size(), "debug");


		allJobs[id] = job{ functionToDo,dep }; // Wlock


		if (dep.size() != 0) { // highly likely we have a dependency

			mutex_jobsWaiting.lock();

			for (uint32_t dependantID : dep) {
				// scheduledJobs_waiting[dependantID] creates a new element if needed!!!
				if (allJobs.find(dependantID) != allJobs.end())
					jobsWaiting[dependantID].push_back(id); // Wlock
			}

			mutex_jobsWaiting.unlock();
			mutex_allJobs.unlock();
		}
		else {
			mutex_allJobs.unlock();
			mutex_jobsReady.lock();

			jobsReady.push(id); // Wlock

			mutex_jobsReady.unlock();
		}

		mutex_fullLock.unlock();
		return id;
	}

	uint32_t GetJob(job& job) {

		//std::this_thread::sleep_for(chrono::microseconds(10000));
		/*
		if (!mutex_scheduledJobs_ready.try_lock()) {
			sleep(10000);
			//cout << std::this_thread::get_id() << " !!!!! "<< (allJobs.size()>= JOB_RESERVE)<< "|" << scheduledJobs_ready.size() << "\n";
			return;
		}
		*/
		mutex_jobsReady.lock(); // Wlock
		//cout << "jobs ready: " << scheduledJobs_ready.size() << "\n";
		uint32_t jobID = 0;
		if (!jobsReady.empty()) { // (Rlock)

			// get a job
			{
				//mutex_scheduledJobs_ready.lock(); // moved before if

				jobID = jobsReady.front(); // Rlock
				jobsReady.pop(); // Wlock

				mutex_jobsReady.unlock(); // Wlock
				//DEBUG_OUT(std::this_thread::get_id() << " - " << jobID);
			}
			{
				mutex_allJobs.lock_shared();
				job = allJobs[jobID]; // Rlock
				//allJobs.erase(jobID); // Wlock
				mutex_allJobs.unlock_shared();
			}
		}
		else {
			mutex_jobsReady.unlock(); // Wlock
		}
		return jobID;
	}

	void FinishJob(uint32_t jobID) {

		mutex_fullLock.lock(); // one thread only
		std::vector<uint32_t> waitingOnMe;

		mutex_jobsWaiting.lock();// _shared();
		if (jobsWaiting.find(jobID) != jobsWaiting.end()) {
			waitingOnMe = std::vector<uint32_t>(jobsWaiting[jobID]); // Rlock
			mutex_jobsWaiting.unlock();// _shared();
		}
		else {

			DEBUG_OUT(jobID << "|1" << "-:-", "debug");
			mutex_allJobs.lock();
			//cout << allJobs.size() << " 0> ";
			allJobs.erase(jobID); // Wlock - muss existieren
			//cout << allJobs.size() << "\n";
			mutex_allJobs.unlock();
			DEBUG_OUT(jobID << "|2" << "-:-", "debug");
			mutex_jobsWaiting.unlock();// _shared();

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
				mutex_jobsReady.lock();
				jobsReady.push(id); // Wlock
				mutex_jobsReady.unlock();
				/*
				mutex_allJobs.lock();
				if (allJobs.find(id) != allJobs.end())
					allJobs[id].ísReady = true;
				else {
					DEBUG_OUT("NOOOOOOOOOOOOOO", "debug");
				}
				mutex_allJobs.unlock();
				*/
			}
		}

		{
			mutex_jobsWaiting.lock();
			jobsWaiting.erase(jobID); // Wlock
			mutex_jobsWaiting.unlock();
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

	//#################################################################

	namespace Worker {
		void Run() {

			job j;
			uint32_t jobID = GetJob(j);
			if (jobID != 0) {

				//cout << std::this_thread::get_id() << "_2\n";
				// do job
				j.functionToDo();
				//j.finished = true;

				DEBUG_OUT(jobID << " fin1", "debug");

				//cout << std::this_thread::get_id() << "_3\n";
				// tell that you finished
				FinishJob(jobID);
				DEBUG_OUT(jobID << " fin2", "debug");

				//cout << std::this_thread::get_id() << "_4\n";
				// now -> repeat
			}
			else {
				std::this_thread::yield();
			}
		}
	}
}

//#####################################################################################################################

namespace Serial {
	uint64_t loops = 0;
#ifdef RUN_TIMERS
	long long duration_floatingAverage = 0;
#endif // RUN_TIMERS

	void Update(atomic<bool>& isRunning)
	{
#ifdef RUN_TIMERS
		chrono::steady_clock::time_point timeStart = chrono::high_resolution_clock::now();
#endif // RUN_TIMERS

		rmt_ScopedCPUSample(Update, 0);
		UpdateInput();
		UpdatePhysics();
		UpdateCollision();
		UpdateAnimation();
		UpdateParticles();
		UpdateGameElements();
		UpdateRendering();
		UpdateSound();
		loops++;

#ifdef RUN_TIMERS
		long long duration = (chrono::duration_cast<chrono::microseconds>(chrono::high_resolution_clock::now() - timeStart).count());
		duration_floatingAverage /= 10;
		duration_floatingAverage *= 9;
		duration_floatingAverage += (duration / 10);
#endif // RUN_TIMERS
	}
}

namespace Parallel {
#ifdef RUN_TIMERS
	std::mutex mutex_timing;
	long long duration_floatingAverage = 0;
	long long frame_floatingAverage = 0;
	chrono::steady_clock::time_point frame_last;
#endif // RUN_TIMERS

	void Update(atomic<bool>& isRunning)
	{
		//cout << "##";
#ifndef IGNORE_REMOTERY_SHIT
		rmt_ScopedCPUSample(UpdateParallel, 0);
#endif // IGNORE_REMOTERY_SHIT

#ifdef RUN_TIMERS

		chrono::steady_clock::time_point* timeStart = new chrono::steady_clock::time_point;
		std::function<void(void)> timerFunction_start = [timeStart] {
			mutex_timing.lock();
			*timeStart = chrono::high_resolution_clock::now();
			mutex_timing.unlock();
		};
		std::function<void(void)> timerFunction_end = [timeStart] {
			// how long this 'frame' took - frames can be intersecting if added faster than finished
			mutex_timing.lock();
			if (frame_floatingAverage != 0) {
				long long frame = (chrono::duration_cast<chrono::microseconds>(chrono::high_resolution_clock::now() - frame_last).count());
				frame_floatingAverage /= 10;
				frame_floatingAverage *= 9;
				frame_floatingAverage += (frame / 10);
				frame_last = chrono::high_resolution_clock::now();
				DEBUG_OUT(frame_floatingAverage << " |- " << frame, "timing");
			}
			else {
				frame_last = chrono::high_resolution_clock::now();
				frame_floatingAverage = 1;
			}

			long long duration = (chrono::duration_cast<chrono::microseconds>(chrono::high_resolution_clock::now() - *timeStart).count());
			duration_floatingAverage /= 10;
			duration_floatingAverage *= 9;
			duration_floatingAverage += (duration / 10);

			DEBUG_OUT(duration_floatingAverage << " |= " << duration, "timing");
			mutex_timing.unlock();

			Scheduler::mutex_allJobs.lock();
			DEBUG_OUT(Scheduler::allJobs.size(), "temp");
			Scheduler::mutex_allJobs.unlock();

			delete timeStart;
		};

		const uint8_t numberOfJobs = 10;
		std::function<void(void)> jobfunctions[numberOfJobs] =
		{
			timerFunction_start,
			UpdateInput ,UpdatePhysics ,UpdateCollision ,UpdateAnimation, UpdateParticles, UpdateGameElements, UpdateRendering, UpdateSound,
			timerFunction_end
		};
		std::vector<uint32_t> jobDependencies[numberOfJobs] = { {}, {0}, {1}, {2}, {3}, {3}, {2}, {4,5,6}, {0}, {1,7,8} };
		uint32_t jobIDs[numberOfJobs];
#else // !RUN_TIMERS
		const uint8_t numberOfJobs = 8;
		std::function<void(void)> jobfunctions[numberOfJobs] =
		{
			UpdateInput ,UpdatePhysics ,UpdateCollision ,UpdateAnimation, UpdateParticles, UpdateGameElements, UpdateRendering, UpdateSound
		};
		std::vector<uint32_t> jobDependencies[numberOfJobs] = { {}, {0}, {1}, {2}, {2}, {1}, {3,4,5}, {} };
		uint32_t jobIDs[numberOfJobs];
#endif // RUN_TIMERS



		for (int i = 0; i < numberOfJobs; i++) {
			std::vector<uint32_t> dependencies;
			for (int pos : jobDependencies[i])
			{
				dependencies.push_back(jobIDs[pos]);
			}
			while (isRunning) {
				jobIDs[i] = Scheduler::CreateJob(jobfunctions[i], dependencies);
				if (jobIDs[i] != 0) {
					break;
				}
				DEBUG_OUT("allJobs full!!! " << Scheduler::jobsReady.size() << "," << Scheduler::jobsWaiting.size(), "Warning");
				std::this_thread::yield();
				//sleep(100000);
			}
		}
#ifndef MULTITHREADED // run jobs after adding them - no workers
		while (isRunning && !Scheduler::allJobs.empty()) {
			// do next job #TODO
			Scheduler::Worker::Run();
		}
#endif // !MULTITHREADED
	}
}

//#####################################################################################################################

int main()
{
	// init stuff
	Scheduler::allJobs.reserve(JOB_RESERVE);
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


	atomic<bool> isRunning = false;  // set later to send start signal

	thread serial([&isRunning]()
	{
		while (!isRunning)
			std::this_thread::yield(); // wait on start signal
		while (isRunning)
			Serial::Update(isRunning);
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
		while (!isRunning)
			std::this_thread::yield(); // wait on start signal
		while (isRunning) {
			Parallel::Update(isRunning);
			//sleep(1);//5000, 10
			//std::this_thread::sleep_for(chrono::microseconds(0));
		}

	});

#ifdef MULTITHREADED
	thread* workers[THREAD_COUNT];
	for (int i = 0; i < THREAD_COUNT; i++) {
		workers[i] = new thread([&isRunning]()
		{
			while (!isRunning)
				std::this_thread::yield(); // wait on start signal
			while (isRunning)
				Scheduler::Worker::Run();
		});
	}
#endif // MULTITHREADED

	// ##### SEND START SIGNAL #####
	isRunning = true;
	chrono::steady_clock::time_point startTime = chrono::high_resolution_clock::now();

	//cout << allJobs.size();
	cout << "Type anything to quit...\n";
	char c;
	cin >> c;
	cout << "Quitting...\n\n";
	isRunning = false;

	serial.join();
	cout << "serial - joined\n";
	parallel.join();
	cout << "parallel - joined\n";

#ifdef MULTITHREADED
	for (int i = 0; i < THREAD_COUNT; i++) {
		workers[i]->join();
		cout << "workers_" << i << " - joined\n";
	}
#endif // MULTITHREADED

	cout << "\nFinished!!!\n";
#ifdef MULTITHREADED
	cout << "Serial Jobs:   " << Serial::loops*8 << "\n";
	cout << "Parallel Jobs: " << Scheduler::topJobID - Scheduler::allJobs.size() << "\n";
#else
	cout << "Serial Jobs:     " << Serial::loops * 8 << "\n";
	cout << "'Parallel' Jobs: " << Scheduler::topJobID - Scheduler::allJobs.size() << "\n";
#endif // MULTITHREADED
	cout << "during " << (chrono::duration_cast<chrono::microseconds>(chrono::high_resolution_clock::now() - startTime).count()) << " MicroSeconds.\n";

#ifndef IGNORE_REMOTERY_SHIT
	rmt_DestroyGlobalInstance(rmt);
#endif // !IGNORE_REMOTERY_SHIT
}