#pragma once
#include <unistd.h>
#include <fstream>
#include <algorithm>
#include <limits>
#include <chrono>
#include <vector>
#include "../../../Models/Shared/SymbolName.hpp"
#include "../../Allocators/SingletonAllocator.hpp"
#include "../../Containers/StackBuffer.hpp"
#include "../../StringUtils.hpp"
#include "../../TypeConvertUtils.hpp"

namespace LiveProfiler {
	/**
	 * Class used to resolve custom symbol name from per process.
	 * Custom symbol names are read from /tmp/perf-$pid.map.
	 *
	 * Same as LinuxProcessAddressLocator, because custom symbol names may change continuously,
	 * it needs to update under certain conditions, use `forceUpdate` can make it always update.
	 */
	class LinuxProcessCustomSymbolResolver {
	public:
		/** Default parameters */
		static const std::size_t DefaultSymbolNamesUpdateMinInterval = 100;

		/** For FreeListAllocator */
		void freeResources() {
			symbolNames_.clear();
			if (!symbolNamesPathBuffer_.empty()) {
				::unlink(symbolNamesPathBuffer_.data());
			}
		}

		/** For FreeListAllocator */
		void reset(
			pid_t pid,
			const std::shared_ptr<std::string>& path,
			const std::shared_ptr<SingletonAllocator<std::string, SymbolName>>& symbolNameAllocator) {
			pid_ = pid;
			path_ = path;
			symbolNameAllocator_ = symbolNameAllocator;
			symbolNames_.clear();
			symbolNamesUpdated_ = {};
			symbolNamesPathBuffer_.clear();
			line_.clear();
			lastReadOffset_ = 0;
		}

		/**
		 * Resolve custom symbol name from address.
		 * Return nullptr if no symbol name is found.
		 * When `forceUpdate` option is true,
		 * symbol names will be forced to update after first resolve is failed,
		 * it can ensure no newly created symbol name is missed but may reduce performance.
		 */
		std::shared_ptr<SymbolName> resolve(std::size_t address, bool forceUpdate) {
			// first try
			auto symbolName = tryResolve(address);
			if (symbolName != nullptr) {
				return symbolName;
			}
			// load custom symbol names from file, prevent frequent loading
			auto now = std::chrono::high_resolution_clock::now();
			if (forceUpdate || now - symbolNamesUpdated_ > symbolNamesUpdateMinInterval_) {
				updateSymbolNames();
				symbolNamesUpdated_ = now;
				// second try
				symbolName = tryResolve(address);
			}
			return symbolName;
		}

		/** Constructor */
		LinuxProcessCustomSymbolResolver() :
			pid_(0),
			path_(),
			symbolNameAllocator_(),
			symbolNames_(),
			symbolNamesUpdated_(),
			symbolNamesUpdateMinInterval_(
				std::chrono::milliseconds(+DefaultSymbolNamesUpdateMinInterval)),
			symbolNamesPathBuffer_(),
			line_(),
			lastReadOffset_(0) { }

	protected:
		/**
		 * Resolve custom symbol name from address.
		 * Return nullptr if no symbol name is found, no retry.
		 */
		std::shared_ptr<SymbolName> tryResolve(std::size_t address) const {
			// fast check
			if (symbolNames_.empty()) {
				return nullptr;
			}
			// find first symbol that fileOffsetEnd > address
			auto it = std::upper_bound(
				symbolNames_.cbegin(), symbolNames_.cend(), address,
				[](const auto& a, const auto& b) {
					return a < b.fileOffsetEnd;
				});
			if (it == symbolNames_.cend()) {
				return nullptr;
			}
			// check is address >= fileOffsetStart and address < fileOffsetEnd
			const auto& symbolName = *it;
			if (address >= symbolName.fileOffsetStart) {
				return symbolName.symbolName;
			}
			return nullptr;
		}

		/**
		 * Line format:
		 * address          size name(may contains space)
		 * 00007F7DD9DB0480 2d   instance bool [System.Private.CoreLib] dynamicClass::IL_STUB_UnboxingStub()
		 * Use incremental read because the file may change continuously.
		 */
		void updateSymbolNames() {
			// build path
			static const std::string prefix("/tmp/perf-");
			static const std::string suffix(".map");
			if (symbolNamesPathBuffer_.empty()) {
				symbolNamesPathBuffer_.appendStr(prefix.data(), prefix.size());
				symbolNamesPathBuffer_.appendLongLong(pid_);
				symbolNamesPathBuffer_.appendStr(suffix.data(), suffix.size());
				symbolNamesPathBuffer_.appendNullTerminator();
			}
			// parse lines
			// check file.eof() can detect last line that didn't ends with '\n'
			std::ifstream file(symbolNamesPathBuffer_.data());
			if (!file) {
				return; // file does not exist
			}
			file.seekg(lastReadOffset_);
			while (std::getline(file, line_) && !file.eof()) {
				lastReadOffset_ = file.tellg();
				std::uintptr_t startAddress = 0;
				std::size_t symbolSize = 0;
				std::string functionName;
				StringUtils::split(line_,
					[this, &startAddress, &symbolSize, &functionName]
					(auto startIndex, auto, auto count) {
					if (count == 0) {
						unsigned long long startAddressL = 0;
						if (TypeConvertUtils::strToUnsignedLongLong(
							line_.c_str() + startIndex, startAddressL, 16)) {
							startAddress = static_cast<std::uintptr_t>(startAddressL);
						}
					} else if (count == 1) {
						unsigned long long symbolSizeL = 0;
						if (TypeConvertUtils::strToUnsignedLongLong(
							line_.c_str() + startIndex, symbolSizeL, 16)) {
							symbolSize = static_cast<std::size_t>(symbolSizeL);
						}
					} else if (count == 2) {
						// custom function name may contains space so copy till last
						functionName.assign(line_, startIndex, line_.size() - startIndex);
					}
				});
				if (startAddress != 0 && symbolSize != 0 && !functionName.empty()) {
					auto symbolName = symbolNameAllocator_->allocate(std::move(functionName), path_);
					symbolNames_.emplace_back(SymbolNameWithOffset({
						std::move(symbolName),
						static_cast<std::size_t>(startAddress),
						static_cast<std::size_t>(startAddress) + symbolSize
					}));
				}
			}
			// sort symbol names by address
			std::sort(symbolNames_.begin(), symbolNames_.end(), [](auto& a, auto& b) {
				return a.fileOffsetEnd < b.fileOffsetEnd;
			});
		}

		/** Custom symbol name doesn't have a fixed file offset across all resolver */
		struct SymbolNameWithOffset {
			std::shared_ptr<SymbolName> symbolName = nullptr;
			std::size_t fileOffsetStart = 0;
			std::size_t fileOffsetEnd = 0;
		};

	protected:
		pid_t pid_;
		std::shared_ptr<std::string> path_;
		std::shared_ptr<SingletonAllocator<std::string, SymbolName>> symbolNameAllocator_;
		std::vector<SymbolNameWithOffset> symbolNames_;
		std::chrono::high_resolution_clock::time_point symbolNamesUpdated_;
		std::chrono::high_resolution_clock::duration symbolNamesUpdateMinInterval_;
		StackBuffer<128> symbolNamesPathBuffer_;
		std::string line_;
		std::size_t lastReadOffset_;
	};
}

