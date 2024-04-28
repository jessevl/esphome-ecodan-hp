#include "ecodan.h"

#include "esp_log.h"
#include "esphome.h"

#include <HardwareSerial.h>
#include <freertos/task.h>
#include <functional>

#include <mutex>
#include <queue>
#include <thread>

#if ARDUINO_ARCH_ESP32
#include <esp_task_wdt.h>
#endif

namespace esphome {
namespace ecodan 
{
    static constexpr const char *TAG = "ecodan.component";    
#pragma region ESP32Hardware
    TaskHandle_t serialRxTaskHandle = nullptr;

    void IRAM_ATTR serial_rx_isr()
    {
        BaseType_t higherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveIndexedFromISR(serialRxTaskHandle, 0, &higherPriorityTaskWoken);
#if CONFIG_IDF_TARGET_ESP32C3
        portEND_SWITCHING_ISR(higherPriorityTaskWoken);
#else
        portYIELD_FROM_ISR(higherPriorityTaskWoken);
#endif
    }

    TaskHandle_t serialSlaveRxTaskHandle = nullptr;
    void IRAM_ATTR serial_slave_rx_isr()
    {
        BaseType_t higherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveIndexedFromISR(serialSlaveRxTaskHandle, 0, &higherPriorityTaskWoken);
#if CONFIG_IDF_TARGET_ESP32C3
        portEND_SWITCHING_ISR(higherPriorityTaskWoken);
#else
        portYIELD_FROM_ISR(higherPriorityTaskWoken);
#endif
    }

    void init_watchdog()
    {
#if ARDUINO_ARCH_ESP32
        esp_task_wdt_init(30, true); // Reset the board if the watchdog timer isn't reset every 30s.        
#endif
    }

    void add_thread_to_watchdog()
    {
#if ARDUINO_ARCH_ESP32
        esp_task_wdt_add(nullptr);
#endif
    }

    void ping_watchdog()
    {
#if ARDUINO_ARCH_ESP32
        esp_task_wdt_reset();
#endif
    }
#pragma endregion ESP32Hardware    

    void EcodanHeatpump::setup() {
        // ESP_LOGI(TAG, "register services"); 
        // register_service(&EcodanHeatpump::set_target_temperature, "set_target_temperature", {"newTemp", "zone"});
        // register_service(&EcodanHeatpump::set_flow_target_temperature, "set_flow_target_temperature", {"newTemp", "zone"});
        // register_service(&EcodanHeatpump::set_dhw_target_temperature, "set_dhw_target_temperature", {"newTemp"});
        // register_service(&EcodanHeatpump::set_dhw_mode, "set_dhw_mode", {"mode"});
        // register_service(&EcodanHeatpump::set_dhw_force, "set_dhw_force", {"on"});
        // register_service(&EcodanHeatpump::set_power_mode, "set_power_mode", {"on"});
        // register_service(&EcodanHeatpump::set_hp_mode, "set_hp_mode", {"mode"});        

        heatpumpInitialized = initialize();
        xTaskCreate(
            [](void* o){ static_cast<EcodanHeatpump*>(o)->serial_rx_thread(); },
            "serial_rx_task",
            8*1024,
            this,
            5,
            NULL);

            if (slaveActive) {
                xTaskCreate(
                    [](void* o){ static_cast<EcodanHeatpump*>(o)->serial_slave_rx_thread(); },
                    "serial_slave_rx_task",
                    8*1024,
                    this,
                    5,
                    NULL);     
            }
    }


    void EcodanHeatpump::publish_state(const std::string& sensorKey, float sensorValue) {
        auto sensor_it = sensors.find(sensorKey);
        if (sensor_it != sensors.end()) {
            sensor_it->second->publish_state(sensorValue);
        } 
        else 
        {
            ESP_LOGI(TAG, "Could not publish state of sensor '%s' with value: '%f'", sensorKey.c_str(), sensorValue);
        }
    }

    void EcodanHeatpump::publish_state(const std::string& sensorKey, const std::string& sensorValue) {        
        auto textSensor_it = textSensors.find(sensorKey);
        if (textSensor_it != textSensors.end()) {
            textSensor_it->second->publish_state(sensorValue);
        }
        else 
        {
            ESP_LOGI(TAG, "Could not publish state of sensor '%s' with value: '%s'", sensorKey.c_str(), sensorValue.c_str());
        }
    }

    void EcodanHeatpump::update() {        
        //ESP_LOGI(TAG, "Update() on core %d", xPortGetCoreID());
        if (heatpumpInitialized)
            handle_loop();            
    }

    void EcodanHeatpump::dump_config() {
        ESP_LOGI(TAG, "config"); 
    }

#pragma region Configuration

    void EcodanHeatpump::set_rx(int rx) { 
        serialRxPort = rx; 
    }

    void EcodanHeatpump::set_tx(int tx) { 
        serialTxPort = tx; 
    }
    
    void EcodanHeatpump::set_slave_rx(int rx) { 
        slaveRxPort = rx; 
    }

    void EcodanHeatpump::set_slave_tx(int tx) { 
        slaveTxPort = tx; 
    }
#pragma endregion Configuration

#pragma region Init

    bool EcodanHeatpump::initialize()
    {
        ESP_LOGI(TAG, "Initializing HeatPump with serial rx: %d, tx: %d", (int8_t)serialRxPort, (int8_t)serialTxPort);

        pinMode(serialRxPort, INPUT_PULLUP);
        pinMode(serialTxPort, OUTPUT);

        if (slaveRxPort > 0 && slaveTxPort > 0) {
            pinMode(slaveRxPort, INPUT_PULLUP);
            pinMode(slaveTxPort, OUTPUT);
            slaveActive = true;
        }

        delay(25); // There seems to be a window after setting the pin modes where trying to use the UART can be flaky, so introduce a short delay

        master.begin(2400, SERIAL_8E1, serialRxPort, serialTxPort);
        if (slaveActive) 
            slave.begin(2400, SERIAL_8E1, slaveRxPort, slaveTxPort);
    
        if (!is_connected())
            begin_connect();
        init_watchdog();        
        return true;
    }

    void EcodanHeatpump::handle_loop()
    {         
        if (!is_connected() && !master.available())
        {
            static auto last_attempt = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (now - last_attempt > std::chrono::seconds(10))
            {
                last_attempt = now;
                if (!begin_connect())
                {
                    ESP_LOGI(TAG, "Failed to start heatpump connection proceedure...");
                }
            }            
            return;
        }
        else if (is_connected())
        {
            // Update status the first time we enter this condition, then every 30s thereafter.
            auto now = std::chrono::steady_clock::now();
            static auto last_update = now - std::chrono::seconds(40);
            if (now - last_update > std::chrono::seconds(20))
            {
                last_update = now;
                if (!begin_get_status())
                {
                    ESP_LOGI(TAG, "Failed to begin heatpump status update!");
                }         
            }
        }
    }

    bool EcodanHeatpump::is_connected()
    {
        return connected;
    }


#pragma endregion Init

#pragma region Commands

    void EcodanHeatpump::set_room_temperature(float newTemp, esphome::ecodan::SetZone zone)
    {
        // if (newTemp > get_max_thermostat_temperature())
        // {
        //     ESP_LOGI(TAG, "Thermostat setting exceeds maximum allowed!");
        //     return;
        // }

        // if (newTemp < get_min_thermostat_temperature())
        // {
        //     ESP_LOGI(TAG, "Thermostat setting is lower than minimum allowed!");
        //     return;
        // }

        Message cmd{MsgType::SET_CMD, SetType::BASIC_SETTINGS};
        cmd[1] = SET_SETTINGS_FLAG_ZONE_TEMPERATURE;
        cmd[2] = static_cast<uint8_t>(zone);

        auto flag = status.HeatingCoolingMode == Status::HpMode::COOL_ROOM_TEMP
            ? static_cast<uint8_t>(Status::HpMode::COOL_ROOM_TEMP) : static_cast<uint8_t>(Status::HpMode::HEAT_ROOM_TEMP);

        if (zone == SetZone::BOTH || zone == SetZone::ZONE_1) {
            cmd[6] = flag;
            cmd.set_float16(newTemp, 10);
        }
            
        if (zone == SetZone::BOTH || zone == SetZone::ZONE_2) {
            cmd[7] = flag;
            cmd.set_float16(newTemp, 12);
        }

        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};
            cmdQueue.emplace(std::move(cmd));
        }

        if (!dispatch_next_cmd())
        {
            ESP_LOGI(TAG, "command dispatch failed for temperature setting!");
            return;
        }

        return;
    }

    void EcodanHeatpump::set_flow_target_temperature(float newTemp, esphome::ecodan::SetZone zone)
    {
        // if (newTemp > get_max_flow_target_temperature(status.hp_mode_as_string()))
        // {
        //     ESP_LOGI(TAG, "Flow temperature setting exceeds maximum allowed (%f)!", get_max_flow_target_temperature(status.hp_mode_as_string()));
        //     return;
        // }

        // if (newTemp < get_min_flow_target_temperature(status.hp_mode_as_string()))
        // {
        //     ESP_LOGI(TAG, "Flow temperature setting is lower than minimum allowed (%f)!", get_min_flow_target_temperature(status.hp_mode_as_string()));
        //     return;
        // }

        Message cmd{MsgType::SET_CMD, SetType::BASIC_SETTINGS};
        cmd[1] = SET_SETTINGS_FLAG_ZONE_TEMPERATURE;
        cmd[2] = static_cast<uint8_t>(zone);
        
        auto flag = status.HeatingCoolingMode == Status::HpMode::COOL_FLOW_TEMP
            ? static_cast<uint8_t>(Status::HpMode::COOL_FLOW_TEMP) : static_cast<uint8_t>(Status::HpMode::HEAT_FLOW_TEMP);

        if (zone == SetZone::BOTH || zone == SetZone::ZONE_1) {
            cmd[6] = flag;
            cmd.set_float16(newTemp, 10);
        }
            
        if (zone == SetZone::BOTH || zone == SetZone::ZONE_2) {
            cmd[7] = flag;
            cmd.set_float16(newTemp, 12);
        }
            
        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};
            cmdQueue.emplace(std::move(cmd));
        }

        if (!dispatch_next_cmd())
        {
            ESP_LOGI(TAG, "command dispatch failed for flow target temperature setting!");
            return;
        }

        return;
    }

    void EcodanHeatpump::set_dhw_target_temperature(float newTemp)
    {

        if (newTemp > get_max_dhw_temperature())
        {
            ESP_LOGI(TAG, "DHW setting exceeds maximum allowed (%s)!", get_max_dhw_temperature());
            return;
        }

        if (newTemp < get_min_dhw_temperature())
        {
            ESP_LOGI(TAG, "DHW setting is lower than minimum allowed (%s)!", get_min_dhw_temperature());
            return;
        }

        Message cmd{MsgType::SET_CMD, SetType::BASIC_SETTINGS};
        cmd[1] = SET_SETTINGS_FLAG_DHW_TEMPERATURE;
        cmd.set_float16(newTemp, 8);

        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};
            cmdQueue.emplace(std::move(cmd));
        }

        if (!dispatch_next_cmd())
        {
            ESP_LOGI(TAG, "command dispatch failed for DHW temperature setting!");
            return;
        }

        return;
    }

    void EcodanHeatpump::set_dhw_mode(std::string mode)
    {
        Status::DhwMode dhwMode = Status::DhwMode::NORMAL;

        if (mode == "Off")
            return set_dhw_force(false);
        else if (mode == "Normal")
            dhwMode = Status::DhwMode::NORMAL;
        else if (mode == "Eco")
            dhwMode = Status::DhwMode::ECO;
        else
            return;

        Message cmd{MsgType::SET_CMD, SetType::BASIC_SETTINGS};
        cmd[1] = SET_SETTINGS_FLAG_DHW_MODE;
        cmd[5] = static_cast<uint8_t>(dhwMode);

        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};
            cmdQueue.emplace(std::move(cmd));
        }

        if (!dispatch_next_cmd())
        {
            ESP_LOGI(TAG, "command dispatch failed for DHW temperature setting!");
            return;
        }

        return;
    }

    void EcodanHeatpump::set_dhw_force(bool on)
    {   
        Message cmd{MsgType::SET_CMD, SetType::DHW_SETTING};
        cmd[1] = SET_SETTINGS_FLAG_MODE_TOGGLE;
        cmd[3] = on ? 1 : 0; // bit[3] of payload is DHW force, bit[2] is Holiday mode.

        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};
            cmdQueue.emplace(std::move(cmd));
        }

        if (!dispatch_next_cmd())
        {
            ESP_LOGI(TAG, "command dispatch failed for DHW force setting!");
            return;
        }

        return;
    }

    void EcodanHeatpump::set_holiday(bool on)
    {   
        Message cmd{MsgType::SET_CMD, SetType::DHW_SETTING};
        cmd[1] = SET_SETTINGS_FLAG_HOLIDAY_MODE_TOGGLE;
        cmd[4] = on ? 1 : 0;

        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};
            cmdQueue.emplace(std::move(cmd));
        }

        if (!dispatch_next_cmd())
        {
            ESP_LOGI(TAG, "command dispatch failed for holiday mode setting!");
            return;
        }

        return;
    }

    void EcodanHeatpump::set_power_mode(bool on)
    {
        Message cmd{MsgType::SET_CMD, SetType::BASIC_SETTINGS};
        cmd[1] = SET_SETTINGS_FLAG_MODE_TOGGLE;
        cmd[3] = on ? 1 : 0;
        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};
            cmdQueue.emplace(std::move(cmd));
        }

        if (!dispatch_next_cmd())
        {
            ESP_LOGI(TAG, "command dispatch failed for DHW force setting!");
            return;
        }

        return;
    }

    void EcodanHeatpump::set_hp_mode(int mode)
    {
        Message cmd{MsgType::SET_CMD, SetType::BASIC_SETTINGS};
        cmd[1] = SET_SETTINGS_FLAG_HP_MODE;
        cmd[6] = mode;

        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};
            cmdQueue.emplace(std::move(cmd));
        }

        if (!dispatch_next_cmd())
        {
            ESP_LOGI(TAG, "command dispatch failed for heat pump mode setting!");
            return;
        }

        return;
    }


    void EcodanHeatpump::clear_command_queue()
    {
        std::lock_guard<std::mutex> lock{cmdQueueMutex};

        while (!cmdQueue.empty())
            cmdQueue.pop();
    }

    bool EcodanHeatpump::dispatch_next_cmd()
    {
        Message msg;
        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};

            if (cmdQueue.empty())
            {
                return true;
            }

            msg = std::move(cmdQueue.front());
            cmdQueue.pop();
        }

        if (!serial_tx(master, msg))
        {
            ESP_LOGI(TAG, "Unable to dispatch status update request, flushing queued requests...");

            clear_command_queue();

            connected = false;
            return false;
        }

        return true;
    }

    bool EcodanHeatpump::begin_connect()
    {
        Message cmd{MsgType::CONNECT_CMD};
        char payload[2] = {0xCA, 0x01};
        cmd.write_payload(payload, sizeof(payload));

        //ESP_LOGI(TAG, "Attempt to tx CONNECT_CMD!");
        if (!serial_tx(master, cmd))
        {
            ESP_LOGI(TAG, "Failed to tx CONNECT_CMD!");
            return false;
        }

        return true;
    }

    bool EcodanHeatpump::begin_get_status()
    {
        {
            std::unique_lock<std::mutex> lock{cmdQueueMutex, std::try_to_lock};

            if (!lock)
            {
                ESP_LOGI(TAG, "Unable to acquire lock for status query, owned by another thread!");
                delay(1);
            }

            if (!cmdQueue.empty())
            {
                ESP_LOGI(TAG, "command queue was not empty when queueing status query: %u", cmdQueue.size());

                while (!cmdQueue.empty())
                    cmdQueue.pop();
            }

            cmdQueue.emplace(MsgType::GET_CMD, GetType::DEFROST_STATE);
            cmdQueue.emplace(MsgType::GET_CMD, GetType::COMPRESSOR_FREQUENCY);
            cmdQueue.emplace(MsgType::GET_CMD, GetType::FORCED_DHW_STATE);
            cmdQueue.emplace(MsgType::GET_CMD, GetType::HEATING_POWER);
            cmdQueue.emplace(MsgType::GET_CMD, GetType::TEMPERATURE_CONFIG);
            cmdQueue.emplace(MsgType::GET_CMD, GetType::SH_TEMPERATURE_STATE);
            cmdQueue.emplace(MsgType::GET_CMD, GetType::DHW_TEMPERATURE_STATE_A);
            cmdQueue.emplace(MsgType::GET_CMD, GetType::DHW_TEMPERATURE_STATE_B);
            cmdQueue.emplace(MsgType::GET_CMD, GetType::ACTIVE_TIME);
            cmdQueue.emplace(MsgType::GET_CMD, GetType::FLOW_RATE);
            cmdQueue.emplace(MsgType::GET_CMD, GetType::MODE_FLAGS_A);
            cmdQueue.emplace(MsgType::GET_CMD, GetType::MODE_FLAGS_B);
            cmdQueue.emplace(MsgType::GET_CMD, GetType::ENERGY_USAGE);
            cmdQueue.emplace(MsgType::GET_CMD, GetType::ENERGY_DELIVERY);
        }

        return dispatch_next_cmd();
    }

#pragma endregion Commands

#pragma region Misc
    std::string EcodanHeatpump::get_device_model()
    {
        return std::string("Ecodan Air Source Heat Pump");
    }

    float EcodanHeatpump::get_temperature_step()
    {
        return temperatureStep;
    }

    float EcodanHeatpump::get_min_thermostat_temperature()
    {
        return 8.0f;
    }

    float EcodanHeatpump::get_max_thermostat_temperature()
    {
        return 28.0f;
    }

    // From FTC6 installation manual ("DHW max. temp.")
    float EcodanHeatpump::get_min_dhw_temperature()
    {
        return 40.0f;
    }

    float EcodanHeatpump::get_max_dhw_temperature()
    {
        return 60.0f;
    }

    // From FTC6 installation manual ("Zone heating/cooling min. temp.")
    float EcodanHeatpump::get_min_flow_target_temperature(std::string mode)
    {
        std::string coolMode = "Cool Flow Temperature";
        return (coolMode == mode) ? 5.0f : 20.0f;
    }

    float EcodanHeatpump::get_max_flow_target_temperature(std::string mode)
    {
        std::string coolMode = "Cool Flow Temperature";
        return (coolMode == mode) ? 25.0f : 60.0f;
    }

#pragma endregion Misc


#pragma region Serial
    bool EcodanHeatpump::serial_tx(HardwareSerial& serial, Message& msg)
    {
        if (!serial)
        {
            ESP_LOGE(TAG, "Serial connection unavailable for tx");
            return false;
        }

        if (serial.availableForWrite() < msg.size())
        {
            ESP_LOGI(TAG, "Serial tx buffer size: %u", serial.availableForWrite());
            return false;
        }

        msg.set_checksum();
        serial.write(msg.buffer(), msg.size());
        //serial.flush(true);
        //ESP_LOGI(TAG, "written:  %d", res);
        //ESP_LOGV(TAG, msg.debug_dump_packet().c_str());

        return true;
    }
    
    void EcodanHeatpump::resync_rx(HardwareSerial& serial)
    {
        // while (serial.available() > 0)
        //     serial.read();

        static char buffer[1024];
        int i = 0;

        while (serial.available() > 0 && serial.peek() != HEADER_MAGIC_A) {
            buffer[i++] = serial.read();
        }

        if (i > 0) {
            Message cmd{MsgType::ACK_RES};
            cmd.write_payload(buffer, i);
            ESP_LOGW(TAG, cmd.debug_dump_packet().c_str());
        }

        clear_command_queue();
    }

    bool EcodanHeatpump::serial_rx(HardwareSerial& serial, Message& msg)
    {
        if (!serial)
        {
            ESP_LOGE(TAG, "Serial connection unavailable for rx");
            return false;
        }

        if (serial.available() < HEADER_SIZE)
        {
            const TickType_t maxBlockingTime = pdMS_TO_TICKS(1000);
            ulTaskNotifyTakeIndexed(0, pdTRUE, maxBlockingTime);

            // We were woken by an interrupt, but there's not enough data available
            // yet on the serial port for us to start processing it as a packet.
            if (serial.available() < HEADER_SIZE)
                return false;
        }
            
        // Scan for the start of an Ecodan packet.
        if (serial.peek() != HEADER_MAGIC_A)
        {
            ESP_LOGE(TAG, "Dropping serial data, header magic mismatch");
            resync_rx(serial);
            return false;
        }

        if (serial.readBytes(msg.buffer(), HEADER_SIZE) < HEADER_SIZE)
        {
            ESP_LOGI(TAG, "Serial port header read failure!");
            resync_rx(serial);
            return false;
        }

        msg.increment_write_offset(HEADER_SIZE);

        if (!msg.verify_header())
        {
            ESP_LOGI(TAG, "Serial port message appears invalid, skipping payload wait...");
            resync_rx(serial);
            return false;
        }

        // It shouldn't take long to receive the rest of the payload after we get the header.
        size_t remainingBytes = msg.payload_size() + CHECKSUM_SIZE;
        auto startTime = std::chrono::steady_clock::now();
        while (serial.available() < remainingBytes)
        {
            delay(1);

            if (std::chrono::steady_clock::now() - startTime > std::chrono::seconds(30))
            {
                ESP_LOGI(TAG, "Serial port message could not be received within 30s (got %u / %u bytes)", serial.available(), remainingBytes);
                resync_rx(serial);
                return false;
            }
        }

        if (serial.readBytes(msg.payload(), remainingBytes) < remainingBytes)
        {
            ESP_LOGI(TAG, "Serial port payload read failure!");
            resync_rx(serial);
            return false;
        }

        msg.increment_write_offset(msg.payload_size()); // Don't count checksum byte.

        if (!msg.verify_checksum())
        {
            resync_rx(serial);
            return false;
        }

        //ESP_LOGV(TAG, msg.debug_dump_packet().c_str());

        return true;
    }

    void EcodanHeatpump::handle_set_response(Message& res)
    {
        if (res.type() != MsgType::SET_RES)
        {
            ESP_LOGI(TAG, "Unexpected set response type: %#x", static_cast<uint8_t>(res.type()));
        }
    }

    void EcodanHeatpump::handle_get_response(Message& res)
    {
        {
            std::lock_guard<Status> lock{status};

            switch (res.payload_type<GetType>())
            {
            case GetType::DEFROST_STATE:
                status.DefrostActive = res[3] != 0;
                publish_state("mode_defrost", status.DefrostActive ? "On" : "Off");
                break;
            case GetType::COMPRESSOR_FREQUENCY:
                status.CompressorFrequency = res[1];
                publish_state("compressor_frequency", status.CompressorFrequency);
                break;
            case GetType::FORCED_DHW_STATE:
                status.DhwForcedActive = res[7] != 0 && res[5] == 0; // bit 5 -> 7 is normal dhw, 0 - forced dhw
                publish_state("mode_dhw_forced", status.DhwForcedActive ? "On" : "Off");
                break;
            case GetType::HEATING_POWER:
                status.OutputPower = res[6];
                status.BoosterActive = res[4] == 2;
                publish_state("output_power", status.OutputPower);
                publish_state("mode_booster", status.BoosterActive ? "On" : "Off");
                break;
            case GetType::TEMPERATURE_CONFIG:
                status.Zone1SetTemperature = res.get_float16(1);
                status.Zone2SetTemperature = res.get_float16(3);
                status.Zone1FlowTemperatureSetPoint = res.get_float16(5);
                status.Zone2FlowTemperatureSetPoint = res.get_float16(7);
                status.LegionellaPreventionSetPoint = res.get_float16(9);
                status.DhwTemperatureDrop = res.get_float8_v2(11);
                status.MaximumFlowTemperature = res.get_float8_v3(12);
                status.MinimumFlowTemperature = res.get_float8_v3(13);

                publish_state("z1_room_temp_target", status.Zone1SetTemperature);
                publish_state("z2_room_temp_target", status.Zone2SetTemperature);
                publish_state("z1_flow_temp_target", status.Zone1FlowTemperatureSetPoint);
                publish_state("z2_flow_temp_target", status.Zone2FlowTemperatureSetPoint);

                publish_state("legionella_prevention_temp", status.LegionellaPreventionSetPoint);
                publish_state("dhw_flow_temp_drop", status.DhwTemperatureDrop);
                
                // min/max flow

                break;
            case GetType::SH_TEMPERATURE_STATE:
                status.Zone1RoomTemperature = res.get_float16(1);
                if (res.get_u16(3) != 0xF0C4) // 0xF0C4 seems to be a sentinel value for "not reported in the current system"
                    status.Zone2RoomTemperature = res.get_float16(3);
                else
                    status.Zone2RoomTemperature = 0.0f;
                status.OutsideTemperature = res.get_float8(11);
                
                publish_state("z1_room_temp", status.Zone1RoomTemperature);
                publish_state("z2_room_temp", status.Zone2RoomTemperature);
                publish_state("outside_temp", status.OutsideTemperature);                
                break;
            case GetType::DHW_TEMPERATURE_STATE_A:
                status.DhwFeedTemperature = res.get_float16(1);
                status.DhwReturnTemperature = res.get_float16(4);
                status.DhwTemperature = res.get_float16(7);

                publish_state("hp_feed_temp", status.DhwFeedTemperature);
                publish_state("hp_return_temp", status.DhwReturnTemperature);
                publish_state("dhw_temp", status.DhwTemperature);
                break;
            case GetType::DHW_TEMPERATURE_STATE_B:
                status.BoilerFlowTemperature = res.get_float16(1);
                status.BoilerReturnTemperature = res.get_float16(4);

                publish_state("boiler_flow_temp", status.BoilerFlowTemperature);
                publish_state("boiler_return_temp", status.BoilerReturnTemperature);
                break;
            case GetType::ACTIVE_TIME:
                status.Runtime = res.get_float24_v2(3);
                publish_state("runtime", status.Runtime);
                //ESP_LOGI(TAG, res.debug_dump_packet().c_str());
                break;
            case GetType::FLOW_RATE:
                status.FlowRate = res[12];
                publish_state("flow_rate", status.FlowRate);
                break;
            case GetType::MODE_FLAGS_A:
                status.set_power_mode(res[3]);
                status.set_operation_mode(res[4]);
                status.set_dhw_mode(res[5]);
                status.set_heating_cooling_mode(res[6]);
                status.DhwFlowTemperatureSetPoint = res.get_float16(8);
                status.RadiatorFlowTemperatureSetPoint = res.get_float16(12);

                publish_state("mode_power", status.power_as_string());
                publish_state("mode_operation", status.operation_as_string());
                publish_state("mode_dhw", status.dhw_mode_as_string());
                publish_state("mode_heating_cooling", status.hp_mode_as_string());

                publish_state("dhw_flow_temp_target", status.DhwFlowTemperatureSetPoint);
                publish_state("sh_flow_temp_target", status.RadiatorFlowTemperatureSetPoint);
                break;
            case GetType::MODE_FLAGS_B:
                status.HolidayMode = res[4] > 0;

                status.ProhibitDhw = res[5] != 0;
                status.ProhibitHeatingZ1 = res[6] != 0;
                status.ProhibitCoolingZ1 = res[7] != 0;
                status.ProhibitHeatingZ2 = res[8] != 0;
                status.ProhibitCoolingZ2 = res[9] != 0;
                
                publish_state("mode_holiday", status.HolidayMode ? "On" : "Off");
                publish_state("mode_prohibit_dhw", status.ProhibitDhw ? "On" : "Off");
                publish_state("mode_prohibit_heating_z1", status.ProhibitHeatingZ1 ? "On" : "Off");
                publish_state("mode_prohibit_cool_z1", status.ProhibitCoolingZ1 ? "On" : "Off");
                publish_state("mode_prohibit_heating_z2", status.ProhibitHeatingZ2 ? "On" : "Off");
                publish_state("mode_prohibit_cool_z2", status.ProhibitCoolingZ2 ? "On" : "Off");
                break;
            case GetType::ENERGY_USAGE:
                status.EnergyConsumedHeating = res.get_float24(4);
                status.EnergyConsumedCooling = res.get_float24(7);
                status.EnergyConsumedDhw = res.get_float24(10);

                publish_state("heating_consumed", status.EnergyConsumedHeating);
                publish_state("cool_consumed", status.EnergyConsumedCooling);
                publish_state("dhw_consumed", status.EnergyConsumedDhw);
                break;
            case GetType::ENERGY_DELIVERY:
                status.EnergyDeliveredHeating = res.get_float24(4);
                status.EnergyDeliveredCooling = res.get_float24(7);
                status.EnergyDeliveredDhw = res.get_float24(10);

                publish_state("heating_delivered", status.EnergyDeliveredHeating);
                publish_state("cool_delivered", status.EnergyDeliveredCooling);
                publish_state("dhw_delivered", status.EnergyDeliveredDhw);
                
                publish_state("heating_cop", status.EnergyConsumedHeating > 0.0f ? status.EnergyDeliveredHeating / status.EnergyConsumedHeating : 0.0f);
                publish_state("cool_cop", status.EnergyConsumedCooling > 0.0f ? status.EnergyDeliveredCooling / status.EnergyConsumedCooling : 0.0f);
                publish_state("dhw_cop", status.EnergyConsumedDhw > 0.0f ? status.EnergyDeliveredDhw / status.EnergyConsumedDhw : 0.0f);

                break;
            default:
                ESP_LOGI(TAG, "Unknown response type received on serial port: %u", static_cast<uint8_t>(res.payload_type<GetType>()));
                break;
            }
        }

        if (!dispatch_next_cmd())
        {
            ESP_LOGI(TAG, "Failed to dispatch status update command!");
        }
    }

    void EcodanHeatpump::handle_connect_response(Message& res)
    {
        ESP_LOGI(TAG, "connection reply received from heat pump");

        connected = true;
    }

    void EcodanHeatpump::handle_ext_connect_response(Message& res)
    {
        ESP_LOGI(TAG, "Unexpected extended connection response!");
    }

    void EcodanHeatpump::serial_rx_thread()
    {        
        add_thread_to_watchdog();
        // Wake the serial RX thread when the serial RX GPIO pin changes (this may occur during or after packet receipt)
        serialRxTaskHandle = xTaskGetCurrentTaskHandle();

        {
            attachInterrupt(digitalPinToInterrupt(serialRxPort), serial_rx_isr, FALLING);
        }        
        
        while (true)
        {   
            //ESP_LOGI(TAG, "handle response on core %d", xPortGetCoreID());
            ping_watchdog();            
            handle_response();            
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    void EcodanHeatpump::serial_slave_rx_thread()
    {        
        add_thread_to_watchdog();
        // Wake the serial RX thread when the serial RX GPIO pin changes (this may occur during or after packet receipt)
        serialSlaveRxTaskHandle = xTaskGetCurrentTaskHandle();

        {
            attachInterrupt(digitalPinToInterrupt(slaveRxPort), serial_slave_rx_isr, FALLING);
        }        
        
        while (true)
        {   
            //ESP_LOGI(TAG, "handle response on core %d", xPortGetCoreID());
            ping_watchdog();            
            handle_slave_response();            
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    void EcodanHeatpump::handle_response() {
        Message res;
        if (!master.available() || !serial_rx(master, res))
            return;
        
        switch (res.type())
        {
            case MsgType::SET_RES:
                handle_set_response(res);
                break;
            case MsgType::GET_RES:
                handle_get_response(res);
                break;
            case MsgType::CONNECT_RES:
                handle_connect_response(res);
                break;
            case MsgType::EXT_CONNECT_RES:
                handle_ext_connect_response(res);
                break;
            default:
                ESP_LOGI(TAG, "Unknown serial message type received: %#x", static_cast<uint8_t>(res.type()));
                break;
        }

        if (slaveActive) {
            // mirror response to slave
            //ESP_LOGI(TAG, "Heatpump -> Master -> Slave (response proxy)");
            ESP_LOGI(TAG, res.debug_dump_packet().c_str());
            serial_tx(slave, res);
        }
    }

    void EcodanHeatpump::handle_slave_response() {
        Message res;
        if (!slave.available() || !serial_rx(slave, res))
            return;

        if (slaveActive) {
            // mirror commands to master
            //ESP_LOGI(TAG, "Slave -> Master -> Heatpump (request proxy)");
            if (res.type() == MsgType::SET_CMD) {
                ESP_LOGW(TAG, res.debug_dump_packet().c_str());
            }
            else {
                ESP_LOGI(TAG, res.debug_dump_packet().c_str());
            }
            
            serial_tx(master, res);

            if (res.type() == MsgType::CONNECT_CMD) {
                //{ .Hdr { fc, 7a, 2, 7a, 1 } .Payload { 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } .Chk { 0 } }
                Message cmd{MsgType::CONNECT_RES, false};
                // char payload[2] = {0x00, 0x09};
                // cmd.write_payload(payload, sizeof(payload));
                ESP_LOGW(TAG, cmd.debug_dump_packet().c_str());
                serial_tx(slave, cmd);
            }
            else if (res.type() == MsgType::GET_CMD && res[0] ==  0x15) {
                // bit 1 = pump running on/off
                // bit 6 = 3 way vale on/off
                Message cmd{MsgType::GET_RES, true};
                char payload[0x10] = { 0x15, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00 };
                cmd.write_payload(payload, sizeof(payload));
                ESP_LOGW(TAG, cmd.debug_dump_packet().c_str());
                serial_tx(slave, cmd);
            }
            else if (res.type() == MsgType::GET_CMD && res[0] ==  0x13) {
                // runtime
                Message cmd{MsgType::GET_RES, true};
                char payload[0x10] = { 0x13, 0x00, 0x00, 0x0F, 0x00, 0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
                cmd.write_payload(payload, sizeof(payload) );
                ESP_LOGW(TAG, cmd.debug_dump_packet().c_str());
                serial_tx(slave, cmd);
            }
            else if (res.type() == MsgType::GET_CMD && res[0] ==  0x0B) {
                // 11 = oudside temp = 14
                // 3 = zone 2 temp 0xF0, 0xC4 == not in system
                // 1 = zone 1 temp = 21
                // 8 = Refrigerant liquid temperature = 21
                Message cmd{MsgType::GET_RES, true};
                char payload[0x10] = { 0x0B, 0x08, 0x34, 0xF0, 0xC4, 0xF0, 0xC4, 0x0B, 0x09, 0x34, 0x74, 0x6C, 0x00, 0x00, 0x00, 0x00 };
                cmd.write_payload(payload, sizeof(payload) );
                ESP_LOGW(TAG, cmd.debug_dump_packet().c_str());
                serial_tx(slave, cmd);
            }
            else if (res.type() == MsgType::GET_CMD && res[0] ==  0x05) {
                // mitsubishi Heat source status
                // 4 = 2 == booster heater
                // 6 = output power
                Message cmd{MsgType::GET_RES, true};
                char payload[0x10] = { 0x05, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03 };
                cmd.write_payload(payload, sizeof(payload) );
                ESP_LOGW(TAG, cmd.debug_dump_packet().c_str());
                serial_tx(slave, cmd);
            }                        
        }
    }

#pragma endregion Serial

} // namespace ecodan
} // namespace esphome