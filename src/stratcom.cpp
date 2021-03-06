/******************************************************************************
 * Copyright (c) 2010-2014 Andreas Weis <der_ghulbus@ghulbus-inc.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *****************************************************************************/

#include <stratcom.h>

#include <hidapi.h>

#include <memory>
#include <new>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
    /*  Magic Constants
     */
    const unsigned short HID_VENDOR_ID  = 0x045e;
    const unsigned short HID_PRODUCT_ID = 0x0033;
    /***/

    /** Generic RAII wrapper for hidapi resource handles.
     * @tparam T Type that is being wrapped.
     * @tparam Deleter Deleter function invoked on the guarded resource when the wrapper is destroyed.
     */
    template<typename T, void(*Deleter)(T*)>
    class hidapi_resource_wrapper
    {
    private:
        std::unique_ptr<T, void(*)(T*)> m_ptr;
    public:
        hidapi_resource_wrapper()
            :m_ptr(nullptr, [](T*) {})
        {
        }

        hidapi_resource_wrapper(T* dev)
            :m_ptr(dev, Deleter)
        {
        }

        operator T*()
        {
            return m_ptr.get();
        }

        operator T const*() const
        {
            return m_ptr.get();
        }

        T* operator->()
        {
            return m_ptr.get();
        }

        T const* operator->() const
        {
            return m_ptr.get();
        }

        bool operator!() const
        {
            return !m_ptr;
        }
    };

    /** Wrapped hidapi types.
     */
    typedef hidapi_resource_wrapper<hid_device, hid_close> hid_device_wrapper;
    typedef hidapi_resource_wrapper<hid_device_info, hid_free_enumeration> hid_device_info_wrapper;

    /** HID feature reports.
     * This is used to set and query the state of the device LEDs.
     * For a detailed description, take a look at the
     * stratcom_flush_button_led_state() and
     * stratcom_set_led_blink_interval() functions.
     */
    struct feature_report {
        std::uint8_t b0;
        std::uint8_t b1;
        std::uint8_t b2;
    };

    /** HID Input Report.
     * This is used to query the state of buttons and axes. It consists
     * of 7 bytes. For detailed description, take a look at the
     * evaluateInputReport() function.
     */
    struct input_report {
        std::uint8_t b0;
        std::uint8_t b1;
        std::uint8_t b2;
        std::uint8_t b3;
        std::uint8_t b4;
        std::uint8_t b5;
        std::uint8_t b6;
    };
}

/** \internal Definition of the opaque stratcom_device_ struct.
 */
struct stratcom_device_ {
    hid_device_wrapper device;                          ///< underlying hidapi device.
    std::uint16_t led_button_state;                     ///< cached state of the device leds.
    struct blink_state_T {
        std::uint8_t on_time;
        std::uint8_t off_time;
    } blink_state;                                      ///< cached state of the device led blink state.
    bool led_button_state_has_unflushed_changes;        ///< true if the cached led state has unflushed changes.
    stratcom_input_state input_state;                   ///< device input state obtained by read_input* functions.

    stratcom_device_(hid_device* dev)
        :device(dev), led_button_state(0), led_button_state_has_unflushed_changes(true)
    {
        std::memset(&input_state, 0, sizeof(input_state));
        blink_state.on_time = 0;
        blink_state.off_time = 0;
    }
};


stratcom_return stratcom_init()
{
    return (hid_init() == 0) ? STRATCOM_RET_SUCCESS : STRATCOM_RET_ERROR;
}

void stratcom_shutdown()
{
    hid_exit();
}

stratcom_device* stratcom_open_device()
{
    hid_device_info_wrapper dev_info_list(hid_enumerate(HID_VENDOR_ID, HID_PRODUCT_ID));
    stratcom_device* ret = nullptr;
    if(dev_info_list) {
        ret = stratcom_open_device_on_path(dev_info_list->path);
    }
    return ret;
}

stratcom_device* stratcom_open_device_on_path(char const* device_path)
{
    auto dev = hid_open_path(device_path);
    if (dev) {
        auto ret = new (std::nothrow) stratcom_device(dev);
        if(ret) {
            stratcom_read_button_led_state(ret);
            stratcom_read_led_blink_intervals(ret);
        }
        return ret;
    }
    return nullptr;
}

void stratcom_close_device(stratcom_device* device)
{
    delete device;
}

stratcom_led_state stratcom_get_button_led_state(stratcom_device* device,
                                                 stratcom_button_led led)
{
    if((led == STRATCOM_LEDBUTTON_ALL) || (led == STRATCOM_LEDBUTTON_NONE)) {
        return STRATCOM_LED_OFF;
    } else if((device->led_button_state & led) != 0) {
        return STRATCOM_LED_ON;
    } else if((device->led_button_state & (led << 1) ) != 0) {
        return STRATCOM_LED_BLINK;
    }
    return STRATCOM_LED_OFF;
}

stratcom_return stratcom_set_button_led_state(stratcom_device* device,
                                   stratcom_button_led led, stratcom_led_state state)
{
    stratcom_set_button_led_state_without_flushing(device, led, state);
    return stratcom_flush_button_led_state(device);
}

void stratcom_set_button_led_state_without_flushing(stratcom_device* device,
                                                    stratcom_button_led led,
                                                    stratcom_led_state state)
{
    auto const button_mask = static_cast<std::uint16_t>(led);
    switch(state) {
    case STRATCOM_LED_BLINK:
        device->led_button_state &= (~button_mask);                //< clear LED_ON bits
        device->led_button_state |= (button_mask << 1);            //< set LED_BLINK bits
        break;
    case STRATCOM_LED_ON:
        device->led_button_state &= (~(button_mask << 1));         //< clear LED_BLINK bits
        device->led_button_state |= button_mask;                   //< set LED_ON bits
        break;
    case STRATCOM_LED_OFF:
        device->led_button_state &= (~button_mask);                //< clear LED_ON bits
        device->led_button_state &= (~(button_mask << 1));         //< clear LED_BLINK bits
        break;
    }
    device->led_button_state_has_unflushed_changes = true;
}

stratcom_return stratcom_flush_button_led_state(stratcom_device* device)
{
    /** \internal
     * LEDs are set by sending a feature report of the following form:
     * b0 = 0x01
     * b1 = LED state Buttons 1-4
     * b1 = LED state Buttons 1-6
     * Each button occupies two bits: A low bit that indicates a lit LED and a higher bit indicating
     * a blinking LED. Those are mutually exclusive, attempting to set both will result in the 
     * feature report being rejected.
     * Therefore, the stratcom_device keeps track of LED states in the led_button_state member which mimics the
     * bitmask send to the device.
     * Values of the stratcom_button_led enum correspond to the bitmasks for LED On bits in led_button_state.
     */
    feature_report report;
    report.b0 = 0x01;
    report.b1 = (device->led_button_state & 0xff);
    report.b2 = ((device->led_button_state >> 8) & 0xff);
    if(hid_send_feature_report(device->device, &report.b0, sizeof(report)) != sizeof(report)) {
        return STRATCOM_RET_ERROR;
    }
    device->led_button_state_has_unflushed_changes = false;
    return STRATCOM_RET_SUCCESS;
}

int stratcom_led_state_has_unflushed_changes(stratcom_device* device)
{
    return device->led_button_state_has_unflushed_changes;
}

void stratcom_get_led_blink_interval(stratcom_device* device,
                                     uint8_t* out_on_time, uint8_t* out_off_time)
{
    *out_on_time = device->blink_state.on_time;
    *out_off_time = device->blink_state.off_time;
}

stratcom_return stratcom_set_led_blink_interval(stratcom_device* device, uint8_t on_time, uint8_t off_time)
{
    /** \internal
     * Blinking speed is set by sending a feature request of the following form:
     * b0 = 0x02
     * b1 = LED on time
     * b2 = LED off time
     * Blinking speed is the same for all LEDs.
     */
    feature_report report;
    report.b0 = 0x02;
    report.b1 = on_time;
    report.b2 = off_time;
    if(hid_send_feature_report(device->device, &report.b0, sizeof(report)) != sizeof(report)) {
        return STRATCOM_RET_ERROR;
    }
    return STRATCOM_RET_SUCCESS;
}

stratcom_return stratcom_read_button_led_state(stratcom_device* device)
{
    feature_report rep;
    rep.b0 = 0x01;
    int const res = hid_get_feature_report(device->device, reinterpret_cast<unsigned char*>(&rep), sizeof(rep));
    if(res != sizeof(rep)) {
        return STRATCOM_RET_ERROR;
    }
    device->led_button_state = static_cast<std::uint16_t>(rep.b1) |
                               (static_cast<std::uint16_t>(rep.b2) << 8);
    device->led_button_state_has_unflushed_changes = false;
    return STRATCOM_RET_SUCCESS;
}

stratcom_return stratcom_read_led_blink_intervals(stratcom_device* device)
{
    feature_report rep;
    rep.b0 = 0x02;
    int const res = hid_get_feature_report(device->device, reinterpret_cast<unsigned char*>(&rep), sizeof(rep));
    if(res != sizeof(rep)) {
        return STRATCOM_RET_ERROR;
    }
    device->blink_state.on_time = rep.b1;
    device->blink_state.off_time = rep.b2;
    return STRATCOM_RET_SUCCESS;
}

stratcom_button_led stratcom_get_led_for_button(stratcom_button button)
{
    switch(button) {
    case STRATCOM_BUTTON_1: return STRATCOM_LEDBUTTON_1;
    case STRATCOM_BUTTON_2: return STRATCOM_LEDBUTTON_2;
    case STRATCOM_BUTTON_3: return STRATCOM_LEDBUTTON_3;
    case STRATCOM_BUTTON_4: return STRATCOM_LEDBUTTON_4;
    case STRATCOM_BUTTON_5: return STRATCOM_LEDBUTTON_5;
    case STRATCOM_BUTTON_6: return STRATCOM_LEDBUTTON_6;
    case STRATCOM_BUTTON_REC:  return STRATCOM_LEDBUTTON_REC;
    default: break;
    }
    return STRATCOM_LEDBUTTON_NONE;
}

namespace {
    stratcom_return evaluateInputReport(input_report const& report, stratcom_input_state& input_state)
    {
        if(report.b0 != 0x01)
        {
            return STRATCOM_RET_ERROR;
        }

        /** \internal
         * Button State.
         * The button state is contained in the b5 and b6 fields of the report.
         * We combine those to a stratcom_button_word as follows:
         *    0000 6666 5555 5555
         * Where the upper nibble of b6 contains information on the slider state
         * and is therefore zeroed out in the stratcom_button_word.
         * Applying a mask from the stratcom_button enum on the stratcom_button_word gives the
         * state of that button: 1 if the button is pressed; 0 otherwise
         */
        input_state.buttons = (((report.b6 & 0x0F) << 8) | report.b5);

        /** \internal
         * Slider State.
         * The Slider state is stored in the upper 4 bits of b6.
         * The corresponding bit patterns are:
         *  SLIDER1 = 0x30,
         *  SLIDER2 = 0x20,
         *  SLIDER3 = 0x10
         */
        if((report.b6 & 0x30) == 0x30) {
            input_state.slider = STRATCOM_SLIDER_1;
        } else {
            input_state.slider = ((report.b6 & 0x20) ? STRATCOM_SLIDER_2 : STRATCOM_SLIDER_3);
        }

        /** \internal
         * Axis State.
         * The state of the 3 axes is saved in the b1, b2 and b3 fields.
         * Each axis takes up 10 bits, allowing values from -512 to +511.
         * The bit pattern is as follows:
         *   {--b1---}  {--b2---}  {--b3---}  {--b4---}
         *   XXXX XXXX  YYYY YYXX  ZZZZ YYYY  00ZZ ZZZZ
         * Where bits in higher fields are also higher-order bits for the
         * raw axis data (e.g. X-bits in b2 are high bits for X-data while
         * Y-bits in b2 are low bits for Y-data).
         * The upper 2 bits of b4 are unused and should always be 0.
         *
         * The raw axis data ranges from -512 to +511 with negative values
         * encoded in two's complement.
         */
        input_state.axisX = ( ((report.b2 & 0x03) << 8) | (report.b1) );
        if(input_state.axisX & 0x200) { input_state.axisX = -( (input_state.axisX ^ 0x3FF) + 1); }
        input_state.axisY = ( ((report.b3 & 0x0f) << 6) | ((report.b2 & 0xfc) >> 2) );
        if(input_state.axisY & 0x200) { input_state.axisY = -( (input_state.axisY ^ 0x3FF) + 1); }
        input_state.axisZ = ( ((report.b4 & 0x3f) << 4) | ((report.b3 & 0xf0) >> 4) );
        if(input_state.axisZ & 0x200) { input_state.axisZ = -( (input_state.axisZ ^ 0x3FF) + 1); }

        return STRATCOM_RET_SUCCESS;
    }
}

stratcom_return stratcom_read_input(stratcom_device* device)
{
    hid_set_nonblocking(device->device, false);
    input_report report;
    int const res = hid_read(device->device, &report.b0, sizeof(report));
    if(res == sizeof(report)) {
        return evaluateInputReport(report, device->input_state);
    } else {
        return STRATCOM_RET_ERROR;
    }
}

stratcom_return stratcom_read_input_with_timeout(stratcom_device* device, int timeout_milliseconds)
{
    hid_set_nonblocking(device->device, false);
    input_report report;
    int const res = hid_read_timeout(device->device, &report.b0, sizeof(report), timeout_milliseconds);
    if(res == sizeof(report)) {
        return evaluateInputReport(report, device->input_state);
    } else if(res != 0) {
        return STRATCOM_RET_ERROR;
    }
    return STRATCOM_RET_NO_DATA;
}

stratcom_return stratcom_read_input_non_blocking(stratcom_device* device)
{
    hid_set_nonblocking(device->device, true);
    input_report report;
    int const res = hid_read(device->device, &report.b0, sizeof(report));
    if(res == sizeof(report)) {
        return evaluateInputReport(report, device->input_state);
    } else if(res != 0) {
        return STRATCOM_RET_ERROR;
    }
    return STRATCOM_RET_NO_DATA;
}

stratcom_input_state stratcom_get_input_state(stratcom_device* device)
{
    return device->input_state;
}

int stratcom_is_button_pressed(stratcom_device* device, stratcom_button button)
{
    return (device->input_state.buttons & button);
}

stratcom_axis_word stratcom_get_axis_value(stratcom_device* device, stratcom_axis axis)
{
    switch(axis) {
    case STRATCOM_AXIS_X: return device->input_state.axisX;
    case STRATCOM_AXIS_Y: return device->input_state.axisY;
    case STRATCOM_AXIS_Z: return device->input_state.axisZ;
    default: break;
    }
    return 0;
}

stratcom_slider_state stratcom_get_slider_state(stratcom_device* device)
{
    return device->input_state.slider;
}

stratcom_button stratcom_iterate_buttons_range_begin()
{
    return STRATCOM_BUTTON_1;
}

stratcom_button stratcom_iterate_buttons_range_end()
{
    return STRATCOM_BUTTON_NONE;
}

stratcom_button stratcom_iterate_buttons_range_increment(stratcom_button button)
{
    switch(button) {
    case STRATCOM_BUTTON_1:      return STRATCOM_BUTTON_2;
    case STRATCOM_BUTTON_2:      return STRATCOM_BUTTON_3;
    case STRATCOM_BUTTON_3:      return STRATCOM_BUTTON_4;
    case STRATCOM_BUTTON_4:      return STRATCOM_BUTTON_5;
    case STRATCOM_BUTTON_5:      return STRATCOM_BUTTON_6;
    case STRATCOM_BUTTON_6:      return STRATCOM_BUTTON_PLUS;
    case STRATCOM_BUTTON_PLUS:   return STRATCOM_BUTTON_MINUS;
    case STRATCOM_BUTTON_MINUS:  return STRATCOM_BUTTON_SHIFT1;
    case STRATCOM_BUTTON_SHIFT1: return STRATCOM_BUTTON_SHIFT2;
    case STRATCOM_BUTTON_SHIFT2: return STRATCOM_BUTTON_SHIFT3;
    case STRATCOM_BUTTON_SHIFT3: return STRATCOM_BUTTON_REC;
    case STRATCOM_BUTTON_REC:    return STRATCOM_BUTTON_NONE;
    default: break;
    }
    return STRATCOM_BUTTON_NONE;
}

stratcom_input_event* stratcom_create_input_events_from_states(stratcom_input_state* old_state,
                                                               stratcom_input_state* new_state)
{
    stratcom_input_event* ret = nullptr;

    try {
        if(old_state->slider != new_state->slider) {
            auto ev = new stratcom_input_event;
            ev->type = STRATCOM_INPUT_EVENT_SLIDER;
            ev->desc.slider.status = new_state->slider;
            ev->next = ret;
            ret = ev;
        }

        auto evaluate_axis_states = [&ret](stratcom_axis_word old_val, stratcom_axis_word new_val, stratcom_axis axis) {
            if(old_val != new_val) {
                auto ev = new stratcom_input_event;
                ev->type = STRATCOM_INPUT_EVENT_AXIS;
                ev->desc.axis.axis = axis;
                ev->desc.axis.status = new_val;
                ev->next = ret;
                ret = ev;
            }
        };
        evaluate_axis_states(old_state->axisX, new_state->axisX, STRATCOM_AXIS_X);
        evaluate_axis_states(old_state->axisY, new_state->axisY, STRATCOM_AXIS_Y);
        evaluate_axis_states(old_state->axisZ, new_state->axisZ, STRATCOM_AXIS_Z);

        if(old_state->buttons != new_state->buttons) {
            for(auto b = stratcom_iterate_buttons_range_begin(); b != stratcom_iterate_buttons_range_end();
                b = stratcom_iterate_buttons_range_increment(b))
            {
                if((old_state->buttons & b) != (new_state->buttons & b)) {
                    auto ev = new stratcom_input_event;
                    ev->type = STRATCOM_INPUT_EVENT_BUTTON;
                    ev->desc.button.button = b;
                    ev->desc.button.status = ((new_state->buttons & b) == 0) ? 0 : 1;
                    ev->next = ret;
                    ret = ev;
                }
            }
        }
    } catch (std::bad_alloc&) { if(ret) { stratcom_free_input_events(ret); ret = nullptr; } }

    return ret;
}

stratcom_input_event* stratcom_append_input_events_from_states(stratcom_input_event* events,
                                                               stratcom_input_state* old_state,
                                                               stratcom_input_state* new_state)
{
    auto new_events = stratcom_create_input_events_from_states(old_state, new_state);
    auto event_it = events;
    while(event_it->next) { event_it = event_it->next; }
    event_it->next = new_events;
    return new_events;
}

void stratcom_free_input_events(stratcom_input_event* events)
{
    while(events) {
        auto to_delete = events;
        events = events->next;
        delete to_delete;
    }
}
