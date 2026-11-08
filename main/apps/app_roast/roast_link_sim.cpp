/*
 * Simulated roaster for practising the UI without the machine.
 * Same register semantics as the BLE link; a crude fluid-bed thermal model with an on-board PID.
 */
#include "roast_link.h"
#include <esp_timer.h>
#include <cmath>
#include <cstdlib>
#include <map>

namespace roast {

class SimLink : public RoasterLink {
public:
    bool begin() override
    {
        _t0 = _last = now();
        _state      = LinkState::Ready;
        return true;
    }
    void end() override
    {
        _state = LinkState::Off;
    }
    LinkState state() override
    {
        return _state;
    }
    std::string deviceName() override
    {
        return "Simulator";
    }
    std::string lastError() override
    {
        return "";
    }
    std::vector<ScanEntry> scanResults() override
    {
        return {{"Simulator", -40}};
    }
    bool isSimulated() const override
    {
        return true;
    }

    RegSnapshot snapshot() override
    {
        step();
        RegSnapshot s;
        s.reg[2]  = _running && _timerOn ? (int16_t)_timer : 0;
        s.reg[4]  = (int16_t)lround(_bt * 10);
        s.reg[9]  = (int16_t)_target;
        s.reg[11] = (int16_t)lround(_heat * 10);
        s.reg[12] = (int16_t)_fan;
        s.reg[13] = _running ? 1 : 0;
        s.reg[15] = _pid ? 1 : 0;
        s.reg[16] = _running ? (int16_t)_fan : 0;
        s.reg[20] = 1000;
        s.reg[21] = 6;
        s.reg[22] = 1;
        s.reg[23] = 8;
        s.reg[28] = 450;
        s.reg[29] = 1000;
        s.valid   = 0x3FFFFFFC;
        s.lastRxMs = (uint32_t)(now() * 1000);
        s.frames   = (uint32_t)((now() - _t0) * 2);
        return s;
    }

    uint32_t command(const std::vector<RegWrite>& writes) override
    {
        for (auto& w : writes) apply(w.first, w.second);
        uint32_t id = _next++;
        _tx[id]     = TxStatus::Ok;
        return id;
    }
    TxStatus txStatus(uint32_t id) override
    {
        auto it = _tx.find(id);
        return it == _tx.end() ? TxStatus::Failed : it->second;
    }
    void setTarget(uint16_t raw) override
    {
        _target = raw;
    }
    void clearQueue() override
    {
    }

private:
    LinkState _state = LinkState::Off;
    double _t0 = 0, _last = 0;
    double _bt = 26.0, _air = 26.0, _heat = 0, _integ = 0, _timer = 0;
    int _target = 500, _fan = 530;
    bool _running = false, _pid = false, _timerOn = false, _dropped = false;
    uint32_t _next = 1;
    std::map<uint32_t, TxStatus> _tx;

    static double now()
    {
        return esp_timer_get_time() / 1e6;
    }

    void apply(uint16_t a, uint16_t v)
    {
        switch (a) {
            case 2:
                _timer   = 0;
                _timerOn = true;
                _dropped = false;
                break;
            case 9:
                _target = v;
                break;
            case 11:
                if (!_pid) _heat = v / 10.0;
                break;
            case 12:
                _fan = v;
                break;
            case 13:
                if (v == 1) {
                    _running = true;
                    _heat    = 0;
                }
                break;
            case 14:
                if (v == 2) {  // drop: heater off, keep blowing to cool
                    _pid     = false;
                    _heat    = 0;
                    _timerOn = false;
                    _dropped = true;
                } else if (v == 1) {  // stop
                    _running = false;
                    _pid     = false;
                    _heat    = 0;
                    _timerOn = false;
                }
                break;
            case 15:
                _pid   = v == 1;
                _integ = 0;
                break;
            default:
                break;
        }
    }

    void step()
    {
        double t  = now();
        double dt = t - _last;
        if (dt <= 0) return;
        if (dt > 2) dt = 2;
        _last = t;
        if (_timerOn) _timer += dt;

        double fanFrac = _running ? _fan / 1000.0 : 0;
        if (_pid && _running) {
            double err = _target / 10.0 - _bt;
            _integ += err * dt * 0.08;
            if (_integ > 60) _integ = 60;
            if (_integ < -20) _integ = -20;
            double out = 6.0 * err + _integ + 30;
            _heat      = out < 0 ? 0 : (out > 100 ? 100 : out);
        }
        // Inlet air: heater raises it, more airflow dilutes it.
        double airTarget = 26 + (_running ? _heat * 3.3 * (0.55 / (fanFrac > 0.2 ? fanFrac : 0.2)) : 0);
        _air += (airTarget - _air) * (1 - exp(-dt / 6.0));
        double k = _running ? 0.010 * (fanFrac / 0.53) : 0.002;
        double endo = (_bt > 196 && _bt < 203 && _timerOn) ? 0.25 : 0;  // first-crack heat sink
        _bt += ((_air - _bt) * k - endo) * dt;
        _bt += ((rand() % 100) - 50) / 2000.0;
        if (_bt < 20) _bt = 20;
    }
};

RoasterLink* CreateSimLink()
{
    return new SimLink();
}

}  // namespace roast
