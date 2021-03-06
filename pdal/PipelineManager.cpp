/******************************************************************************
* Copyright (c) 2011, Michael P. Gerlek (mpg@flaxen.com)
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following
* conditions are met:
*
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in
*       the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of Hobu, Inc. or Flaxen Geo Consulting nor the
*       names of its contributors may be used to endorse or promote
*       products derived from this software without specific prior
*       written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
* OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
* OF SUCH DAMAGE.
****************************************************************************/

#include <pdal/PipelineManager.hpp>
#include <pdal/StageFactory.hpp>
#include <pdal/PipelineReaderJSON.hpp>
#include <pdal/PDALUtils.hpp>
#include <pdal/util/Algorithm.hpp>
#include <pdal/util/FileUtils.hpp>

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace pdal
{

PipelineManager::PipelineManager() : m_factory(new StageFactory),
    m_tablePtr(new PointTable()), m_table(*m_tablePtr),
    m_progressFd(-1), m_input(nullptr)
{}


PipelineManager::~PipelineManager()
{
    Utils::closeFile(m_input);
}


void PipelineManager::readPipeline(std::istream& input)
{
    std::istreambuf_iterator<char> eos;

    // Read stream into string.
    std::string s(std::istreambuf_iterator<char>(input), eos);

    std::istringstream ss(s);
    PipelineReaderJSON(*this).readPipeline(ss);
}


void PipelineManager::readPipeline(const std::string& filename)
{
    if (FileUtils::extension(filename) == ".json")
    {
        PipelineReaderJSON pipeReader(*this);
        return pipeReader.readPipeline(filename);
    }
    else
    {
        Utils::closeFile(m_input);
        m_input = Utils::openFile(filename);
        if (!m_input)
            throw pdal_error("Can't open file '" + filename + "' as pipeline "
                "input.");
        try
        {
            readPipeline(*m_input);
        }
        catch (const pdal_error& err)
        {
            throw pdal_error(filename + ": " + err.what());
        }
    }
}


Stage& PipelineManager::addReader(const std::string& type)
{
    Stage *reader = m_factory->createStage(type);
    if (!reader)
    {
        std::ostringstream ss;
        ss << "Couldn't create reader stage of type '" << type << "'.";
        throw pdal_error(ss.str());
    }
    reader->setLog(m_log);
    reader->setProgressFd(m_progressFd);
    m_stages.push_back(reader);
    return *reader;
}


Stage& PipelineManager::addFilter(const std::string& type)
{
    Stage *filter = m_factory->createStage(type);
    if (!filter)
    {
        std::ostringstream ss;
        ss << "Couldn't create filter stage of type '" << type << "'.";
        throw pdal_error(ss.str());
    }
    filter->setLog(m_log);
    filter->setProgressFd(m_progressFd);
    m_stages.push_back(filter);
    return *filter;
}


Stage& PipelineManager::addWriter(const std::string& type)
{
    Stage *writer = m_factory->createStage(type);
    if (!writer)
    {
        std::ostringstream ss;
        ss << "Couldn't create writer stage of type '" << type << "'.";
        throw pdal_error(ss.str());
    }
    writer->setLog(m_log);
    writer->setProgressFd(m_progressFd);
    m_stages.push_back(writer);
    return *writer;
}


void PipelineManager::validateStageOptions() const
{
    // Make sure that the options specified are for relevant stages.
    for (auto& si : m_stageOptions)
    {
        const std::string& stageName = si.first;
        auto it = std::find_if(m_stages.begin(), m_stages.end(),
            [stageName](Stage *s)
            { return (s->getName() == stageName ||
                "stage." + s->tag() == stageName); });

        // If the option stage name matches no created stage, then error.
        if (it == m_stages.end())
        {
            std::ostringstream oss;
            oss << "Argument references invalid/unused stage: '" <<
                stageName << "'.";
            throw pdal_error(oss.str());
        }
    }
}


bool PipelineManager::pipelineStreamable() const
{
    bool streamable = false;

    Stage *s = getStage();
    if (s)
        streamable = s->pipelineStreamable();
    return streamable;
}


QuickInfo PipelineManager::preview() const
{
    QuickInfo qi;

    validateStageOptions();
    Stage *s = getStage();
    if (s)
       qi = s->preview();
    return qi;
}


void PipelineManager::prepare() const
{
    validateStageOptions();
    Stage *s = getStage();
    if (s)
       s->prepare(m_table);
}


point_count_t PipelineManager::execute()
{
    prepare();

    Stage *s = getStage();
    if (!s)
        return 0;
    m_viewSet = s->execute(m_table);
    point_count_t cnt = 0;
    for (auto pi = m_viewSet.begin(); pi != m_viewSet.end(); ++pi)
    {
        PointViewPtr view = *pi;
        cnt += view->size();
    }
    return cnt;
}


void PipelineManager::executeStream(StreamPointTable& table)
{
    validateStageOptions();
    Stage *s = getStage();
    if (!s)
        return;

    s->prepare(table);
    s->execute(table);
}


MetadataNode PipelineManager::getMetadata() const
{
    MetadataNode output("stages");

    for (auto s : m_stages)
    {
        output.add(s->getMetadata());
    }
    return output;
}


Stage& PipelineManager::makeReader(const std::string& inputFile,
    std::string driver)
{
    StageCreationOptions ops { inputFile, driver };

    return makeReader(ops);
}


Stage& PipelineManager::makeReader(const std::string& inputFile,
    std::string driver, Options options)
{
    StageCreationOptions ops { inputFile, driver, nullptr, options };

    return makeReader(ops);
}


Stage& PipelineManager::makeReader(StageCreationOptions& o)
{
    if (o.m_driver.empty())
    {
        o.m_driver = StageFactory::inferReaderDriver(o.m_filename);
        if (o.m_driver.empty())
            throw pdal_error("Cannot determine reader for input file: " +
                o.m_filename);
    }
    if (!o.m_filename.empty())
        o.m_options.replace("filename", o.m_filename);

    Stage& reader = addReader(o.m_driver);
    reader.setTag(o.m_tag);
    setOptions(reader, o.m_options);
    return reader;
}


Stage& PipelineManager::makeFilter(const std::string& driver)
{
    StageCreationOptions ops { "", driver };

    return makeFilter(ops);
}


Stage& PipelineManager::makeFilter(const std::string& driver, Options options)
{
    StageCreationOptions ops { "", driver, nullptr, options };

    return makeFilter(ops);
}


Stage& PipelineManager::makeFilter(const std::string& driver, Stage& parent)
{
    StageCreationOptions ops { "", driver, &parent };

    return makeFilter(ops);
}


Stage& PipelineManager::makeFilter(const std::string& driver, Stage& parent,
    Options options)
{
    StageCreationOptions ops { "", driver, &parent, options };

    return makeFilter(ops);
}


Stage& PipelineManager::makeFilter(StageCreationOptions& o)
{
    Stage& filter = addFilter(o.m_driver);
    filter.setTag(o.m_tag);
    setOptions(filter, o.m_options);
    if (o.m_parent)
        filter.setInput(*o.m_parent);
    return filter;
}


Stage& PipelineManager::makeWriter(const std::string& outputFile,
    std::string driver)
{
    StageCreationOptions ops { outputFile, driver };

    return makeWriter(ops);
}


Stage& PipelineManager::makeWriter(const std::string& outputFile,
    std::string driver, Stage& parent)
{
    StageCreationOptions ops { outputFile, driver, &parent };

    return makeWriter(ops);
}


Stage& PipelineManager::makeWriter(const std::string& outputFile,
    std::string driver, Stage& parent, Options options)
{
    StageCreationOptions ops { outputFile, driver, &parent, options };

    return makeWriter(ops);
}


Stage& PipelineManager::makeWriter(const std::string& outputFile,
    std::string driver, Options options)
{
    StageCreationOptions ops { outputFile, driver, nullptr, options };

    return makeWriter(ops);
}


Stage& PipelineManager::makeWriter(StageCreationOptions& o)
{
    if (o.m_driver.empty())
    {
        o.m_driver = StageFactory::inferWriterDriver(o.m_filename);
        if (o.m_driver.empty())
            throw pdal_error("Cannot determine writer for output file: " +
                o.m_filename);
    }

    if (!o.m_filename.empty())
        o.m_options.replace("filename", o.m_filename);

    auto& writer = addWriter(o.m_driver);
    writer.setTag(o.m_tag);
    setOptions(writer, o.m_options);
    if (o.m_parent)
        writer.setInput(*o.m_parent);
    return writer;
}


void PipelineManager::setOptions(Stage& stage, const Options& addOps)
{
    // First apply common options.
    stage.setOptions(m_commonOptions);

    // Apply additional reader/writer options, making sure they replace any
    // common options.
    stage.removeOptions(addOps);
    stage.addOptions(addOps);

    // Apply options provided on the command line, overriding others.
    Options ops = stageOptions(stage);
    stage.removeOptions(ops);
    stage.addOptions(ops);
}


Options PipelineManager::stageOptions(Stage& stage)
{
    Options opts;

    std::string tag = stage.tag();
    if (tag.size())
    {
        tag = "stage." + tag;
        auto oi = m_stageOptions.find(tag);
        if (oi != m_stageOptions.end())
            opts.add(oi->second);
    }
    // Tag-based options options override stagename-based options, so
    // we call addConditional.
    auto oi = m_stageOptions.find(stage.getName());
    if (oi != m_stageOptions.end())
        opts.addConditional(oi->second);
    return opts;
}


std::vector<Stage *> PipelineManager::roots() const
{
    std::vector<Stage *> rlist;

    for (Stage *s : m_stages)
        if (s->getInputs().empty())
            rlist.push_back(s);
    return rlist;
}


std::vector<Stage *> PipelineManager::leaves() const
{
    std::vector<Stage *> llist = m_stages;
    for (Stage *s : m_stages)
        for (Stage *ss : s->getInputs())
           Utils::remove(llist, ss);
    return llist;
}


void PipelineManager::replace(Stage *sOld, Stage *sNew)
{
    Utils::remove(m_stages, sNew);
    for (Stage * & s : m_stages)
    {
        if (s == sOld)
        {
            s = sNew;
            // Copy inputs from the old stage to new one.
            for (Stage *ss : sOld->getInputs())
                sNew->setInput(*ss);
        }
        // Reset the inputs that refer to the replaced stage.
        for (Stage * & ss : s->getInputs())
            if (ss == sOld)
                ss = sNew;
    }
}


void PipelineManager::destroyStage(Stage *s)
{
    if (s)
        m_factory->destroyStage(s);
    else
        m_factory.reset(new StageFactory());
}


} // namespace pdal
