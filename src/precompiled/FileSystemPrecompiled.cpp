/**
 *  Copyright (C) 2021 FISCO BCOS.
 *  SPDX-License-Identifier: Apache-2.0
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * @file FileSystemPrecompiled.cpp
 * @author: kyonRay
 * @date 2021-06-10
 */

#include "FileSystemPrecompiled.h"
#include "Common.h"
#include "PrecompiledResult.h"
#include "Utilities.h"
#include <json/json.h>
#include <boost/algorithm/string/split.hpp>
#include <boost/throw_exception.hpp>

using namespace bcos;
using namespace bcos::executor;
using namespace bcos::storage;
using namespace bcos::precompiled;
using namespace bcos::protocol;

const char* const FILE_SYSTEM_METHOD_LIST = "list(string)";
const char* const FILE_SYSTEM_METHOD_MKDIR = "mkdir(string)";

FileSystemPrecompiled::FileSystemPrecompiled(crypto::Hash::Ptr _hashImpl) : Precompiled(_hashImpl)
{
    name2Selector[FILE_SYSTEM_METHOD_LIST] = getFuncSelector(FILE_SYSTEM_METHOD_LIST, _hashImpl);
    name2Selector[FILE_SYSTEM_METHOD_MKDIR] = getFuncSelector(FILE_SYSTEM_METHOD_MKDIR, _hashImpl);
}

std::string FileSystemPrecompiled::toString()
{
    return "FileSystem";
}

std::shared_ptr<PrecompiledExecResult> FileSystemPrecompiled::call(
    std::shared_ptr<executor::TransactionExecutive> _executive, bytesConstRef _param,
    const std::string&, const std::string&)
{
    uint32_t func = getParamFunc(_param);
    bytesConstRef data = getParamData(_param);
    PRECOMPILED_LOG(DEBUG) << LOG_BADGE("FileSystemPrecompiled") << LOG_DESC("call")
                           << LOG_KV("func", func);

    auto callResult = std::make_shared<PrecompiledExecResult>();
    auto gasPricer = m_precompiledGasFactory->createPrecompiledGas();
    gasPricer->setMemUsed(_param.size());

    if (func == name2Selector[FILE_SYSTEM_METHOD_LIST])
    {
        // list(string)
        listDir(_executive, data, callResult, gasPricer);
    }
    else if (func == name2Selector[FILE_SYSTEM_METHOD_MKDIR])
    {
        // mkdir(string)
        makeDir(_executive, data, callResult, gasPricer);
    }
    else
    {
        PRECOMPILED_LOG(ERROR) << LOG_BADGE("FileSystemPrecompiled")
                               << LOG_DESC("call undefined function!");
    }

    gasPricer->updateMemUsed(callResult->m_execResult.size());
    callResult->setGas(gasPricer->calTotalGas());
    return callResult;
}

void FileSystemPrecompiled::makeDir(
    const std::shared_ptr<executor::TransactionExecutive>& _executive, bytesConstRef& data,
    std::shared_ptr<PrecompiledExecResult> callResult, const PrecompiledGas::Ptr& gasPricer)
{
    // mkdir(string)
    std::string absolutePath;
    auto blockContext = _executive->blockContext().lock();
    auto codec =
        std::make_shared<PrecompiledCodec>(blockContext->hashHandler(), blockContext->isWasm());
    codec->decode(data, absolutePath);
    PRECOMPILED_LOG(DEBUG) << LOG_BADGE("FileSystemPrecompiled") << LOG_KV("mkdir", absolutePath);
    if (!checkPathValid(absolutePath))
    {
        PRECOMPILED_LOG(ERROR) << LOG_BADGE("FileSystemPrecompiled")
                               << LOG_DESC("directory exists");
        callResult->setExecResult(codec->encode(s256((int)CODE_FILE_INVALID_PATH)));
        return;
    }
    auto table = _executive->storage().openTable(absolutePath);
    gasPricer->appendOperation(InterfaceOpcode::OpenTable);
    if (table)
    {
        PRECOMPILED_LOG(ERROR) << LOG_BADGE("FileSystemPrecompiled")
                               << LOG_DESC("file name exists, please check");
        callResult->setExecResult(codec->encode(s256((int)CODE_FILE_ALREADY_EXIST)));
    }
    else
    {
        PRECOMPILED_LOG(TRACE) << LOG_BADGE("FileSystemPrecompiled")
                               << LOG_DESC("directory not exists, recursive build dir")
                               << LOG_KV("path", absolutePath);
        auto buildResult = recursiveBuildDir(_executive, absolutePath);
        auto result = buildResult ? CODE_SUCCESS : CODE_FILE_BUILD_DIR_FAILED;
        getErrorCodeOut(callResult->mutableExecResult(), result, *codec);
    }
}

void FileSystemPrecompiled::listDir(
    const std::shared_ptr<executor::TransactionExecutive>& _executive, bytesConstRef& data,
    std::shared_ptr<PrecompiledExecResult> callResult, const PrecompiledGas::Ptr& gasPricer)
{
    // list(string)
    std::string absolutePath;
    auto blockContext = _executive->blockContext().lock();
    auto codec =
        std::make_shared<PrecompiledCodec>(blockContext->hashHandler(), blockContext->isWasm());
    codec->decode(data, absolutePath);
    PRECOMPILED_LOG(DEBUG) << LOG_BADGE("FileSystemPrecompiled") << LOG_KV("ls", absolutePath);
    if (!checkPathValid(absolutePath))
    {
        PRECOMPILED_LOG(ERROR) << LOG_BADGE("FileSystemPrecompiled")
                               << LOG_DESC("directory exists");
        callResult->setExecResult(codec->encode(s256((int)CODE_FILE_INVALID_PATH)));
        return;
    }
    auto table = _executive->storage().openTable(absolutePath);
    gasPricer->appendOperation(InterfaceOpcode::OpenTable);

    if (table)
    {
        // file exists, query parent dir
        auto parentDirAndBaseName = getParentDirAndBaseName(absolutePath);
        auto parentDir = parentDirAndBaseName.first;
        auto baseName = parentDirAndBaseName.second;
        auto parentTable = _executive->storage().openTable(parentDir);
        assert(parentTable);
        auto baseEntry = parentTable->getRow(baseName);
        if (!baseEntry)
        {
            PRECOMPILED_LOG(ERROR)
                << LOG_BADGE("FileSystemPrecompiled")
                << LOG_DESC("file exists, but not found in parentDir")
                << LOG_KV("parentDir", parentDir) << LOG_KV("fileName", baseName);
            callResult->setExecResult(codec->encode(s256((int)CODE_FILE_NOT_EXIST)));
            return;
        }
        auto type = baseEntry->getField(FS_FIELD_TYPE);
        if (type == FS_TYPE_DIR)
        {
            // directory
            Json::Value subdirectory(Json::arrayValue);
            auto fileNameList = table->getPrimaryKeys({});
            auto fileInfoMap = table->getRows(fileNameList);
            for (size_t i = 0; i < fileNameList.size(); ++i)
            {
                auto fileName = fileNameList[i];
                auto entry = fileInfoMap[i];
                if (!entry)
                {
                    PRECOMPILED_LOG(WARNING)
                        << LOG_BADGE("FileSystemPrecompiled")
                        << LOG_DESC("getRows return null entry") << LOG_KV("fileName", fileName);
                    continue;
                }
                Json::Value file;
                file[FS_KEY_NAME] = fileName;
                file[FS_FIELD_TYPE] = std::string(entry->getField(FS_FIELD_TYPE));
                file[FS_FIELD_EXTRA] = std::string(entry->getField(FS_FIELD_EXTRA));
                subdirectory.append(file);
            }
            Json::FastWriter fastWriter;
            std::string str = fastWriter.write(subdirectory);
            PRECOMPILED_LOG(TRACE)
                << LOG_BADGE("FileSystemPrecompiled") << LOG_DESC("ls dir, return subdirectories");
            callResult->setExecResult(codec->encode(str));
        }
        else
        {
            // contract
            Json::Value fileList(Json::arrayValue);
            Json::Value file;
            file[FS_KEY_NAME] = baseName;
            file[FS_FIELD_TYPE] = std::string(baseEntry->getField(FS_FIELD_TYPE));
            file[FS_FIELD_EXTRA] = std::string(baseEntry->getField(FS_FIELD_EXTRA));
            fileList.append(file);
            Json::FastWriter fastWriter;
            std::string str = fastWriter.write(fileList);
            callResult->setExecResult(codec->encode(str));
        }
    }
    else
    {
        PRECOMPILED_LOG(ERROR) << LOG_BADGE("FileSystemPrecompiled")
                               << LOG_DESC("can't open table of file path")
                               << LOG_KV("path", absolutePath);
        getErrorCodeOut(callResult->mutableExecResult(), CODE_FILE_NOT_EXIST, *codec);
    }
}
