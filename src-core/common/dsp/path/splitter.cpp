#include "splitter.h"
#include <chrono>
#include <volk/volk.h>

namespace dsp
{
    SplitterBlock::SplitterBlock(std::shared_ptr<dsp::stream<complex_t>> input)
        : Block(input)
    {
    }

    // Normal Copy
    void SplitterBlock::add_output(std::string id)
    {
        state_mutex.lock();
        if (outputs.count(id) == 0)
            outputs.insert({id, {std::make_shared<dsp::stream<complex_t>>(), false}});
        state_mutex.unlock();
    }

    void SplitterBlock::del_output(std::string id)
    {
        state_mutex.lock();
        if (outputs.count(id) > 0)
            outputs.erase(id);
        state_mutex.unlock();
    }

    std::shared_ptr<dsp::stream<complex_t>> SplitterBlock::get_output(std::string id)
    {
        if (outputs.count(id) > 0)
            return outputs[id].output_stream;
        else
            return nullptr;
    }

    void SplitterBlock::set_enabled(std::string id, bool enable)
    {
        state_mutex.lock();
        if (outputs.count(id) > 0)
            outputs[id].enabled = enable;
        state_mutex.unlock();
    }

    void SplitterBlock::reset_output(std::string id)
    {
        state_mutex.lock();
        if (outputs.count(id) > 0)
        {
            outputs[id].output_stream = std::make_shared<dsp::stream<complex_t>>();
            outputs[id].enabled = false;
        }
        state_mutex.unlock();
    }

    // VFO
    void SplitterBlock::add_vfo(std::string id, double samplerate, double freq)
    {
        state_mutex.lock();
        if (vfo_outputs.count(id) == 0)
            vfo_outputs.insert({id,
                                {std::make_shared<dsp::stream<complex_t>>(),
                                 false, (float)freq, complex_t(cos(hz_to_rad(freq, samplerate)), sin(hz_to_rad(freq, samplerate)))}});
        state_mutex.unlock();
    }

    void SplitterBlock::del_vfo(std::string id)
    {
        state_mutex.lock();
        if (vfo_outputs.count(id) > 0)
            vfo_outputs.erase(id);
        state_mutex.unlock();
    }

    std::shared_ptr<dsp::stream<complex_t>> SplitterBlock::get_vfo_output(std::string id)
    {
        if (vfo_outputs.count(id) > 0)
            return vfo_outputs[id].output_stream;
        else
            return nullptr;
    }

    void SplitterBlock::set_vfo_enabled(std::string id, bool enable)
    {
        state_mutex.lock();
        if (vfo_outputs.count(id) > 0)
            vfo_outputs[id].enabled = enable;
        state_mutex.unlock();
    }

    void SplitterBlock::reset_vfo(std::string id)
    {
        state_mutex.lock();
        if (vfo_outputs.count(id) > 0)
        {
            vfo_outputs[id].output_stream = std::make_shared<dsp::stream<complex_t>>();
            vfo_outputs[id].enabled = false;
        }
        state_mutex.unlock();
    }

    // Main
    void SplitterBlock::set_main_enabled(bool enable)
    {
        state_mutex.lock();
        enable_main = enable;
        state_mutex.unlock();
    }

    double SplitterBlock::seconds_since_last_input() const
    {
        int64_t last_ns = last_input_ns.load(std::memory_order_relaxed);
        if (last_ns == 0)
            return -1.0;

        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
        return static_cast<double>(now_ns - last_ns) / 1e9;
    }

    void SplitterBlock::work()
    {
        int nsamples = input_stream->read();
        if (nsamples <= 0)
        {
            input_stream->flush();
            return;
        }

        last_input_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count(),
                            std::memory_order_relaxed);

        state_mutex.lock();

        // Main
        if (enable_main)
            memcpy(output_stream->writeBuf, input_stream->readBuf, nsamples * sizeof(complex_t));

        // Copy outputs
        for (auto &o : outputs)
            if (o.second.enabled)
                memcpy(o.second.output_stream->writeBuf, input_stream->readBuf, nsamples * sizeof(complex_t));

        // VFO Outputs
        for (auto &o : vfo_outputs)
        {
            if (o.second.enabled)
            {
                if (o.second.freq == 0)
                    memcpy(o.second.output_stream->writeBuf, input_stream->readBuf, nsamples * sizeof(complex_t));
                else
#if VOLK_VERSION >= 030100
                    volk_32fc_s32fc_x2_rotator2_32fc((lv_32fc_t *)o.second.output_stream->writeBuf, (lv_32fc_t *)input_stream->readBuf, (lv_32fc_t *)&o.second.phase_delta, (lv_32fc_t *)&o.second.phase, nsamples);
#else
                    volk_32fc_s32fc_x2_rotator_32fc((lv_32fc_t *)o.second.output_stream->writeBuf, (lv_32fc_t *)input_stream->readBuf, o.second.phase_delta, (lv_32fc_t *)&o.second.phase, nsamples);
#endif
            }
        }

        input_stream->flush();

        if (enable_main)
            output_stream->swap(nsamples);

        for (auto &o : outputs)
            if (o.second.enabled)
                o.second.output_stream->swap(nsamples);

        for (auto &o : vfo_outputs)
            if (o.second.enabled)
                o.second.output_stream->swap(nsamples);

        state_mutex.unlock();
    }
}
