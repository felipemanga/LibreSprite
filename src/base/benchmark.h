#pragma once

#include <cstdint>
#include <iostream>
#include <source_location>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <vector>

class Benchmark {
    static inline std::atomic<const char*> location {"NO_PROBE"};
public:
  class Probe {
    const char* prevLocation;
  public:
    Probe(std::source_location loc = std::source_location::current()) {
      prevLocation = loc.function_name();
      location.exchange(prevLocation);
    }

    ~Probe() {
      location.exchange(prevLocation);
    }
  };

  class Sampler {
    std::jthread worker;
    std::jthread autologger;
    std::atomic_bool alive;
    std::unordered_map<std::string, uint64_t> counts;
    std::unordered_map<std::string, uint64_t> totalCounts;
    std::mutex countsMutex;

    void work() {
      using namespace std::chrono_literals;
      while (true) {
        std::this_thread::sleep_for(10ms);
        if (!alive)
          return;
        std::string current {location.load()};
        {
          std::lock_guard guard{countsMutex};
          if (auto it = counts.find(current); it != counts.end()) {
              ++it->second;
          } else {
              counts[current] = 1;
          }
        }
      }
    }

    void autolog() {
      using namespace std::chrono_literals;
      while (true) {
        std::this_thread::sleep_for(5s);
        if (!alive)
          return;
        std::cout << "\n--- BENCHMARK ---" << std::endl;
        for (auto& [key, count] : this->count()) {
          std::cout << " - " << key << ": " << count << std::endl;
        }
      }
    }

  public:
    Sampler(bool autoLog = false) : alive{true} {
      worker = std::jthread{[this]{work();}};
      if (autoLog)
        autologger = std::jthread{[this]{autolog();}};
    }

    ~Sampler() {alive = false;}

    std::vector<std::pair<std::string, std::uint64_t>> count(std::size_t maxCount = 20) {
      std::vector<std::pair<std::string, std::uint64_t>> top;
      std::unordered_map<std::string, uint64_t> localCounts;
      {
        std::lock_guard guard{countsMutex};
        std::swap(counts, localCounts);
      }
      for (auto& [key, count] : localCounts) {
        if (auto it = totalCounts.find(key); it != totalCounts.end()) {
          it->second += count;
        } else {
          totalCounts[key] = count;
        }
      }
      for (auto& [key, count] : totalCounts) {
        for (auto it = top.begin(); it != top.end(); ++it) {
          if (count > it->second) {
            top.emplace(it, std::make_pair(key, count));
            goto didInsert;
          }
        }
        top.emplace_back(std::make_pair(key, count));
      didInsert:
        if (top.size() > maxCount) {
            top.pop_back();
        }
      }
      return top;
    }
  };
};
