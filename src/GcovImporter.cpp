// Copyright (C) 2018 xaizek <xaizek@posteo.net>
//
// This file is part of uncov.
//
// uncov is free software: you can redistribute it and/or modify
// it under the terms of version 3 of the GNU Affero General Public License as
// published by the Free Software Foundation.
//
// uncov is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with uncov.  If not, see <http://www.gnu.org/licenses/>.

#include "GcovImporter.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>

#include <cassert>

#include <fstream>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "utils/fs.hpp"
#include "utils/md5.hpp"
#include "utils/strings.hpp"
#include "BuildHistory.hpp"
#include "integration.hpp"

namespace fs = boost::filesystem;

// See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=89961 for information about
// what's wrong with some versions of `gcov` and why binning is needed.

//! First version of `gcov` which has broken `--preserve-paths` option.
static int FirstBrokenGcovVersion = 8;

namespace {
    /**
     * @brief A set of files that should be passed to `gcov` at the same time.
     */
    class Bin {
    public:
        /**
         * @brief Constructs an empty set.
         *
         * @param deduplicateNames Whether to avoid adding name-duplicates.
         */
        Bin(bool deduplicateNames = true) : deduplicateNames(deduplicateNames)
        { }

    public:
        /**
         * @brief Tries to add a file to this bin.
         *
         * @param path Absolute path to the file.
         *
         * @returns `true` if the path was added.
         */
        bool add(const fs::path &path)
        {
            assert(path.is_absolute() && "Paths should be absolute.");

            if (deduplicateNames &&
                !names.emplace(path.filename().string()).second) {
                return false;
            }

            paths.emplace_back(path.string());
            return true;
        }

        /**
         * @brief Retrieves list of paths of this bin.
         *
         * @returns The list.
         */
        const std::vector<std::string> & getPaths() const
        { return paths; }

    private:
        //! Whether no two files should have the same name.
        bool deduplicateNames;
        //! Names of files in this bin if `deduplicateNames` is set.
        std::unordered_set<std::string> names;
        //! Files of this bin.
        std::vector<std::string> paths;
    };
}

GcovInfo::GcovInfo() : employBinning(true)
{
    const std::regex versionRegex("gcov \\(GCC\\) (.*)");

    std::smatch match;

    const std::string version = readProc({ "gcov", "--version" });
    if (std::regex_search(version, match, versionRegex)) {
        const int majorVersion = std::stoi(match[1]);
        employBinning = (majorVersion >= FirstBrokenGcovVersion);
    }
}

void
GcovImporter::setRunner(std::function<runner_f> runner)
{
    getRunner() = std::move(runner);
}

GcovImporter::GcovImporter(const std::string &root,
                           const std::string &covoutRoot,
                           const std::vector<std::string> &exclude,
                           const std::string &prefix)
    : rootDir(normalizePath(fs::absolute(root))),
      prefix(prefix)
{
    for (const std::string &p : exclude) {
        skipPaths.insert(normalizePath(fs::absolute(p, root)));
    }

    std::unordered_set<std::string> extensions = {
        ".h", ".hh", ".hpp", ".hxx",
        ".c", ".cc", ".cpp", ".cxx",
        ".m", ".mm",
    };
    std::unordered_set<std::string> skipDirs = {
        ".git", ".hg", ".svn", // Various version control systems.
        ".deps"                // Dependency tracking of automake.
    };

    std::vector<fs::path> gcnoFiles;
    for (fs::recursive_directory_iterator it(fs::absolute(covoutRoot)), end;
         it != end; ++it) {
        fs::path path = it->path();
        if (fs::is_directory(path)) {
            if (skipDirs.count(path.filename().string())) {
                it.no_push();
                continue;
            }
        } else if (fs::is_regular(path)) {
            // We are looking for *.gcno and not *.gcda, because they are
            // generated even for files that weren't executed (e.g.,
            // `main.cpp`).
            if (path.extension() == ".gcno") {
                gcnoFiles.push_back(path);
            }
        }
    }

    importFiles(std::move(gcnoFiles));

    for (fs::recursive_directory_iterator it(rootDir), end; it != end; ++it) {
        fs::path path = it->path();
        if (fs::is_directory(path)) {
            if (skipDirs.count(path.filename().string())) {
                it.no_push();
            } else if (skipPaths.count(normalizePath(fs::absolute(path)))) {
                it.no_push();
            }
        } else if (extensions.count(path.extension().string())) {
            std::string filePath = makeRelativePath(rootDir, path).string();
            if (mapping.find(filePath) == mapping.end()) {
                std::string contents = readFile(path.string());
                std::string hash = md5(contents);

                int nLines = std::count(contents.cbegin(), contents.cend(),
                                        '\n');

                files.emplace_back(filePath, std::move(hash),
                                   std::vector<int>(nLines, -1));
            }
        }
    }

    for (auto &e : mapping) {
        std::string contents = readFile(root + '/' + e.first);
        std::string hash = md5(contents);

        std::vector<std::string> lines = split(contents, '\n');
        e.second.resize(lines.size(), -1);

        unsigned int i = 0U;
        for (std::string &line : lines) {
            boost::trim_if(line, boost::is_any_of("\r\n \t"));
            if (line == "}" || line == "};") {
                e.second[i] = -1;
            }
            ++i;
        }

        files.emplace_back(e.first, std::move(hash), std::move(e.second));
    }

    mapping.clear();
}

std::vector<File> &&
GcovImporter::getFiles() &&
{
    return std::move(files);
}

void
GcovImporter::importFiles(std::vector<fs::path> gcnoFiles)
{
    std::vector<Bin> bins;

    if (gcovInfo.needsBinning()) {
        // We want to execute the runner for tests even if there are no input
        // files.
        bins.emplace_back();

        for (const fs::path &gcnoFile : gcnoFiles) {
            bool added = false;

            for (Bin &bin : bins) {
                if (bin.add(gcnoFile)) {
                    added = true;
                    break;
                }
            }

            if (!added) {
                bins.emplace_back();
                bins.back().add(gcnoFile);
            }
        }
    } else {
        bins.emplace_back(/*deduplicateNames=*/false);
        Bin &bin = bins.back();
        for (const fs::path &gcnoFile : gcnoFiles) {
            bin.add(gcnoFile);
        }
    }

    for (const Bin &bin : bins) {
        const std::vector<std::string> &paths = bin.getPaths();

        std::vector<std::string> cmd = {
            "gcov", "--preserve-paths", "--intermediate-format", "--"
        };
        cmd.insert(cmd.cend(), paths.cbegin(), paths.cend());

        TempDir tempDir("gcovi");
        std::string tempDirPath = tempDir;
        getRunner()(std::move(cmd), tempDirPath);

        for (fs::recursive_directory_iterator it(tempDirPath), end;
            it != end; ++it) {
            fs::path path = it->path();
            if (fs::is_regular(path) && path.extension() == ".gcov") {
                parseGcov(path.string());
            }
        }
    }
}

void
GcovImporter::parseGcov(const std::string &path)
{
    std::vector<int> *coverage = nullptr;
    std::ifstream in(path);
    for (std::string line; in >> line; ) {
        std::string type, value;
        std::tie(type, value) = splitAt(line, ':');
        if (type == "file") {
            const std::string sourcePath = resolveSourcePath(value);
            coverage = (sourcePath.empty() ? nullptr : &mapping[sourcePath]);
        } else if (coverage != nullptr && type == "lcount") {
            std::vector<std::string> fields = split(value, ',');
            if (fields.size() < 2U) {
                throw std::runtime_error("Not enough fields in lcount: " +
                                         value);
            }

            unsigned int lineNo = std::stoi(fields[0]);
            int count = std::stoi(fields[1]);
            updateCoverage(*coverage, lineNo, count);
        }
    }
}

std::string
GcovImporter::resolveSourcePath(fs::path unresolved)
{
    if (!unresolved.is_absolute()) {
        unresolved = prefix / unresolved;
    }

    fs::path sourcePath = normalizePath(fs::absolute(unresolved,
                                                     rootDir));
    if (!pathIsInSubtree(rootDir, sourcePath) || isExcluded(sourcePath)) {
        return std::string();
    }

    return makeRelativePath(rootDir, sourcePath).string();
}

void
GcovImporter::updateCoverage(std::vector<int> &coverage, unsigned int lineNo,
                             int count)
{
    if (coverage.size() < lineNo) {
        coverage.resize(lineNo, -1);
    }

    int &entry = coverage[lineNo - 1U];
    entry = (entry == -1 ? count : entry + count);
}

bool
GcovImporter::isExcluded(boost::filesystem::path path) const
{
    for (const fs::path &skipPath : skipPaths) {
        if (pathIsInSubtree(skipPath, path)) {
            return true;
        }
    }
    return false;
}

std::function<GcovImporter::runner_f> &
GcovImporter::getRunner()
{
    static std::function<runner_f> runner;
    return runner;
}
