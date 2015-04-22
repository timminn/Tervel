/*
#The MIT License (MIT)
#
#Copyright (c) 2015 University of Central Florida's Computer Software Engineering
#Scalable & Secure Systems (CSE - S3) Lab
#
#Permission is hereby granted, free of charge, to any person obtaining a copy
#of this software and associated documentation files (the "Software"), to deal
#in the Software without restriction, including without limitation the rights
#to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
#copies of the Software, and to permit persons to whom the Software is
#furnished to do so, subject to the following conditions:
#
#The above copyright notice and this permission notice shall be included in
#all copies or substantial portions of the Software.
#
#THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
#AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
#THE SOFTWARE.
#
*/

//  #define KILL_THREAD 1

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#include <chrono>
#include <vector>
#include <thread>

#include <iostream>
#include <atomic>

#include <boost/random.hpp>
#include <boost/generator_iterator.hpp>
#include <gflags/gflags.h>

#include <tervel/algorithms/wf/mcas/mcas.h>
#include <tervel/util/info.h>
#include <tervel/util/thread_context.h>
#include <tervel/util/tervel.h>
#include <tervel/util/memory/hp/hp_element.h>
#include <tervel/util/memory/hp/hp_list.h>

DEFINE_int32(num_threads, 64, "The number of threads to spawn.");
DEFINE_int32(execution_time, 30, "The amount of time to run the tests");
DEFINE_int32(array_length, 32, "The size of the region to test on.");
DEFINE_int32(mcas_size, 16, "The number of words in a mcas operation.");
DEFINE_int32(operation_type, 0, "The type of test to execute"
    "(0: updating sinlge multi-word object"
    ", 1: updating multiple objects"
    ", 2: updating overlapping mult-word updates)");

enum class TestType : size_t {UPDATEOBJECT = 0, UPDATEMULTIOBJECT = 1,
      RANDOMOVERLAPS = 2};

class TestObject {
 public:
  TestObject(int num_threads, int execution_time,
        int array_length, int mcas_size, TestType test_type)
      : execution_time_(execution_time)
      , array_length_(array_length)
      , num_threads_(num_threads)
      , mcas_size_(mcas_size)
      , operation_type_(test_type) {
        shared_memory_ = new std::atomic<void *>[array_length];
      }

  void atomic_add(int passed_count, int failed_count) {
    passed_count_.fetch_add(passed_count);
    failed_count_.fetch_add(failed_count);
  }




  const int execution_time_;
  const int array_length_;
  const int num_threads_;
  const int mcas_size_;
  const TestType operation_type_;

  std::atomic<uint64_t> passed_count_ {0};
  std::atomic<uint64_t> failed_count_ {0};
  std::atomic<void *>* shared_memory_;

  std::atomic<int> ready_count_ {0};
  std::atomic<bool> running_ {true};
  std::atomic<bool> wait_flag_ {true};
};


int array_length, mcas_size;

void run(int thread_id, tervel::Tervel* tervel_obj, TestObject * test_object);
void run_update_object(int start_pos, TestObject * test_data);

int main(int argc, char** argv) {
  std::cout << "start " << std::endl;
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  TestObject test_data(FLAGS_num_threads, FLAGS_execution_time,
        FLAGS_array_length, FLAGS_mcas_size,
        static_cast<TestType>(FLAGS_operation_type) );

  tervel::Tervel tervel_obj(test_data.num_threads_+1);
  tervel::ThreadContext tervel_thread(&tervel_obj);

  // run_update_object(0, &test_data);

  std::vector<std::thread> thread_list;
  for (int i = 0; i < test_data.num_threads_; i++) {
    std::thread temp_thread(run, i, &tervel_obj, &test_data);
    thread_list.push_back(std::move(temp_thread));
  }
  std::cout << "before while" << std::endl;

  while (test_data.ready_count_.load() < test_data.num_threads_) {}

  std::cout << "after while" << std::endl;

  test_data.wait_flag_.store(false);
  std::this_thread::sleep_for(std::chrono::seconds(test_data.execution_time_));
  test_data.running_.store(false);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  printf("Singled Stop!\n");

  std::for_each(thread_list.begin(), thread_list.end(), [](std::thread &t) {
      t.join();
  });

  printf("Completed[Passed: %lu, Failed: %lu]\n",
  test_data.passed_count_.load(), test_data.failed_count_.load());

  for (int i = 0; i < test_data.array_length_; i++) {
    printf("[%d: %p] ", i, test_data.shared_memory_[i].load());
  }

  return 1;
}


void run_update_multible_objects(int thread_id, TestObject * test_data);
void run_RandomOverlaps(int thread_id, TestObject * test_data);
void run_update_object(int thread_id,  int start_pos, TestObject * test_data);

void run(int thread_id, tervel::Tervel* tervel_obj, TestObject * test_data) {
  tervel::ThreadContext tervel_thread(tervel_obj);

  switch (test_data->operation_type_) {
    case TestType::UPDATEOBJECT:
      run_update_object(thread_id, 0, test_data);
      break;

    case TestType::UPDATEMULTIOBJECT:
      run_update_multible_objects(thread_id, test_data);
      break;

    case TestType::RANDOMOVERLAPS:
      run_RandomOverlaps(thread_id, test_data);
      break;

    default:
      printf("Error Non Recongized State Test\n");
  }
}

void * calc_next_value(void * value) {
  uintptr_t temp = reinterpret_cast<uintptr_t>(value);
  temp = (temp + 0x16) & (~3);
  return reinterpret_cast<void *>(temp);
}

void run_update_object(int thread_id, int start_pos, TestObject * test_data) {
  int failed_count = 0;
  int passed_count = 0;

  tervel::algorithms::wf::mcas::MCAS<void *> *mcas;

  test_data->ready_count_.fetch_add(1);
  while (test_data->wait_flag_.load()) {}

  while (test_data->running_.load()) {
    mcas = new tervel::algorithms::wf::mcas::MCAS<void *>(test_data->mcas_size_);

    for (int i = 0; i < test_data->mcas_size_; i++) {
      int var = start_pos + i;

      std::atomic<void *> *address = (&(test_data->shared_memory_)[var]);
      void * expected_value = tervel::algorithms::wf::mcas::read<void *>(address);
      void * new_value = calc_next_value(expected_value);

      bool success = mcas->add_cas_triple(address, expected_value, new_value);
      assert(success);
    }

    if (mcas->execute()) {
      passed_count++;
    } else {
      failed_count++;
    }
    mcas->safe_delete();
  }  // End Execution Loop

  test_data->atomic_add(passed_count, failed_count);
  return;
}

void run_update_object(int start_pos, TestObject * test_data) {
  int failed_count = 0;
  int passed_count = 0;

  tervel::algorithms::wf::mcas::MCAS<void *> *mcas;

  for (int i = 0; i < 20000; i++) {
    mcas = new tervel::algorithms::wf::mcas::MCAS<void *>(test_data->mcas_size_);

    for (int i = 0; i < test_data->mcas_size_; i++) {
      int var = start_pos + i;

      std::atomic<void *> *address = (&(test_data->shared_memory_)[var]);
      void * expected_value = tervel::algorithms::wf::mcas::read<void *>(address);
      void * new_value = calc_next_value(expected_value);

      bool success = mcas->add_cas_triple(address, expected_value, new_value);
      assert(success);
    }

    if (mcas->execute()) {
      passed_count++;
    } else {
      failed_count++;
    }

    mcas->safe_delete();
  }  // End Execution Loop

  test_data->atomic_add(passed_count, failed_count);
  return;
}


void run_update_multible_objects(int thread_id, TestObject * test_data) {
  int failed_count = 0;
  int passed_count = 0;

  int max_start_pos=(test_data->array_length_/test_data->mcas_size_);
  boost::mt19937 rng(thread_id);
  boost::uniform_int<> memory_pos_rand(0, max_start_pos-1);

  tervel::algorithms::wf::mcas::MCAS<void *> *mcas;

  test_data->ready_count_.fetch_add(1);
  while (test_data->wait_flag_.load()) {}

  while (test_data->running_.load()) {
    mcas = new tervel::algorithms::wf::mcas::MCAS<void *>(test_data->mcas_size_);
    int start_pos = memory_pos_rand(rng) * test_data->mcas_size_;

    for (int i = 0; i < mcas_size; i++) {
      int var = start_pos + i;

      std::atomic<void *> *address = (&(test_data->shared_memory_)[var]);
      void * expected_value = tervel::algorithms::wf::mcas::read<void *>(address);
      void * new_value = calc_next_value(expected_value);

      bool success = mcas->add_cas_triple(address, expected_value, new_value);
      assert(success);
    }

    if (mcas->execute()) {
      passed_count++;
    } else {
      failed_count++;
    }

    mcas->safe_delete();
  }  // End Execution Loop

  test_data->atomic_add(passed_count, failed_count);
  return;
}

void run_RandomOverlaps(int thread_id, TestObject * test_data) {
  int failed_count = 0;
  int passed_count = 0;

  boost::mt19937 rng(thread_id);
  boost::uniform_int<> memory_pos_rand(0, test_data->array_length_);
  tervel::algorithms::wf::mcas::MCAS<void *> *mcas;

  test_data->ready_count_.fetch_add(1);
  while (test_data->wait_flag_.load()) {}

  while (test_data->running_.load()) {
    mcas = new tervel::algorithms::wf::mcas::MCAS<void *>(test_data->mcas_size_);

    bool success;
    for (int i = 0; i < mcas_size; i++) {
      do {
        int var = memory_pos_rand(rng);
        std::atomic<void *> *address = (&(test_data->shared_memory_)[var]);
        void * expected_value = tervel::algorithms::wf::mcas::read<void *>(address);
        void * new_value = calc_next_value(expected_value);

        success = mcas->add_cas_triple(address, expected_value, new_value);
      }while(!success);
    }

    if (mcas->execute()) {
      passed_count++;
    } else {
      failed_count++;
    }
    mcas->safe_delete();
  }  // End Execution Loop

  test_data->atomic_add(passed_count, failed_count);
  return;
}