#include <IPv4Layer.h>
#include <Packet.h>
#include <PcapLiveDeviceList.h>
#include <SystemUtils.h>
#include <EthLayer.h>
#include <Layer.h>
#include <memory>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <iostream>

class EtherCATMaster {
public:
    struct SDOMessage {
        uint16_t index;
        uint8_t subindex;
        std::vector<uint8_t> data;
    };

private:
    static constexpr uint16_t ETHERCAT_TYPE = 0x88A4;

    struct EtherCATHeader {
        uint8_t cmd;
        uint8_t idx;
        uint16_t addr;
        uint16_t len;
        uint16_t irq;
    };

    class EtherCATLayer : public pcpp::Layer {
    private:
        std::vector<uint8_t> m_data;
        EtherCATHeader m_header;

    public:
        EtherCATLayer(uint8_t cmd, const std::vector<uint8_t> &data) : Layer() {
            m_header.cmd = cmd;
            m_header.idx = 0;
            m_header.addr = 0;
            m_header.len = static_cast<uint16_t>(data.size());
            m_header.irq = 0;
            m_data = data;
        }

        void parseNextLayer() override {
        }

        std::string toString() const override {
            return "EtherCAT Layer";
        }

        void computeCalculateFields() override {
        }

        size_t getHeaderLen() const override {
            return sizeof(EtherCATHeader) + m_data.size();
        }

        pcpp::OsiModelLayer getOsiModelLayer() const override {
            return pcpp::OsiModelApplicationLayer;
        }
    };

    std::unique_ptr<pcpp::PcapLiveDevice> m_device;
    pcpp::MacAddress m_hostMac;
    pcpp::MacAddress m_slaveMac;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_responseReceived;
    std::vector<uint8_t> m_lastResponse;

    // Packet capture callback handler
    static void onPacketArrives(pcpp::RawPacket *rawPacket, pcpp::PcapLiveDevice *dev, void *cookie) {
        auto *master = static_cast<EtherCATMaster *>(cookie);
        master->handlePacket(rawPacket);
    }

public:
    EtherCATMaster(const std::string &interface_ip) {
        m_device.reset(pcpp::PcapLiveDeviceList::getInstance().getPcapLiveDeviceByIp(interface_ip));
        if (!m_device) {
            throw std::runtime_error("Failed to find network interface");
        }

        if (!m_device->open()) {
            throw std::runtime_error("Failed to open device");
        }

        m_hostMac = m_device->getMacAddress();
        setupPacketCapture();
    }

    ~EtherCATMaster() {
        if (m_device) {
            m_device->stopCapture();
            m_device->close();
        }
    }

    bool connect(const std::string &slave_mac) {
        try {
            m_slaveMac = pcpp::MacAddress(slave_mac);
            return true;
        } catch (const std::exception &e) {
            std::cerr << "Failed to set slave MAC: " << e.what() << std::endl;
            return false;
        }
    }

    bool setOperationMode(uint8_t mode) {
        return writeSDO(0x6060, 0x00, {mode});
    }

    bool setTargetPosition(int32_t position) {
        std::vector<uint8_t> data(4);
        memcpy(data.data(), &position, sizeof(position));
        return writeSDO(0x607A, 0x00, data);
    }

    bool enableDrive() {
        uint16_t controlWord = 0x000F;
        std::vector<uint8_t> data(2);
        memcpy(data.data(), &controlWord, sizeof(controlWord));
        return writeSDO(0x6040, 0x00, data);
    }

    std::optional<int32_t> getActualPosition() {
        auto response = readSDO(0x6064, 0x00);
        if (!response || response->size() < 4) {
            return std::nullopt;
        }

        int32_t position;
        memcpy(&position, response->data(), sizeof(position));
        return position;
    }

private:
    void setupPacketCapture() {
        if (!m_device->startCapture(onPacketArrives, this)) {
            throw std::runtime_error("Failed to start packet capture");
        }
    }

    void handlePacket(pcpp::RawPacket *rawPacket) {
        pcpp::Packet parsedPacket(rawPacket);

        auto *ethLayer = parsedPacket.getLayerOfType<pcpp::EthLayer>();
        if (!ethLayer || ntohs(ethLayer->getEthHeader()->etherType) != ETHERCAT_TYPE) {
            return;
        }

        if (ethLayer->getSourceMac() != m_slaveMac) {
            return;
        }

        // Extract payload data
        auto *payloadLayer = parsedPacket.getLastLayer();
        if (payloadLayer) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastResponse.assign(
                payloadLayer->getData(),
                payloadLayer->getData() + payloadLayer->getDataLen()
            );
            m_responseReceived = true;
            m_cv.notify_one();
        }
    }

    bool writeSDO(uint16_t index, uint8_t subindex, const std::vector<uint8_t> &data) {
        SDOMessage msg{index, subindex, data};
        return sendMessage(msg, true);
    }

    std::optional<std::vector<uint8_t> > readSDO(uint16_t index, uint8_t subindex) {
        SDOMessage msg{index, subindex, {}};
        if (!sendMessage(msg, false)) {
            return std::nullopt;
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_cv.wait_for(lock, std::chrono::seconds(5), [this]() { return m_responseReceived; })) {
            return m_lastResponse;
        }
        return std::nullopt;
    }

    bool sendMessage(const SDOMessage &msg, bool isWrite) {
        pcpp::Packet packet(100);

        // Add Ethernet layer
        pcpp::EthLayer ethLayer(m_hostMac, m_slaveMac, ETHERCAT_TYPE);
        if (!packet.addLayer(&ethLayer)) {
            return false;
        }

        // Prepare SDO data
        std::vector<uint8_t> sdo_data;
        prepareSdoData(msg, isWrite, sdo_data);

        // Add EtherCAT layer
        auto *ecatLayer = new EtherCATLayer(isWrite ? 0x02 : 0x01, sdo_data);
        if (!packet.addLayer(ecatLayer)) {
            return false;
        }

        packet.computeCalculateFields();
        return m_device->sendPacket(&packet);
    }

    void prepareSdoData(const SDOMessage &msg, bool isWrite, std::vector<uint8_t> &output) {
        output.resize(8 + msg.data.size());
        output[0] = isWrite ? 0x23 : 0x40;
        output[1] = msg.index & 0xFF;
        output[2] = (msg.index >> 8) & 0xFF;
        output[3] = msg.subindex;

        if (isWrite) {
            uint32_t size = static_cast<uint32_t>(msg.data.size());
            memcpy(&output[4], &size, sizeof(size));
            memcpy(&output[8], msg.data.data(), msg.data.size());
        }
    }
};

int main() {
    try {
        EtherCATMaster master("192.168.1.100");

        if (!master.connect("00:11:22:33:44:55")) {
            std::cerr << "Failed to connect to slave" << std::endl;
            return 1;
        }

        master.setOperationMode(1);
        master.enableDrive();
        master.setTargetPosition(10000);

        for (int i = 0; i < 100; i++) {
            if (auto position = master.getActualPosition()) {
                std::cout << "Current position: " << *position << std::endl;
            } else {
                std::cout << "Failed to read position" << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
