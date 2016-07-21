/*! @file ZetaRF.cpp
 *
 * @brief This file contains the public functions controlling the Si4455 radio chip.
 *
 * http://www.silabs.com/products/wireless/EZRadio/Pages/Si4455.aspx
 */

#include <Arduino.h>
#include <SPI.h>

#include "ZetaRF.h"
#include "radio_config.h"


#define SI4455_FIFO_SIZE 64
#define RADIO_CTS_TIMEOUT 10000

#define ZETARF_SPI_SETTINGS SPISettings(1000000UL, MSBFIRST, SPI_MODE0)


static const uint8_t DefaultRadioConfigurationDataArray[] = RADIO_CONFIGURATION_DATA_ARRAY;


ZetaRF::ZetaRF(int csPin, int shutdownPin, int irqPin) :
    m_csPin(csPin),
    m_sdnPin(shutdownPin),
    m_irqPin(irqPin),
    m_ctsWentHigh(0),
    m_channelNumber(RADIO_CONFIGURATION_DATA_CHANNEL_NUMBER),
    m_packetLength(RADIO_CONFIGURATION_DATA_RADIO_PACKET_LENGTH),
    m_dataTransmittedFlag(false),
    m_dataAvailableFlag(false),
    m_crcErrorFlag(false),
    m_txFifoAlmostEmptyFlag(false),
    m_rxFifoAlmostFullFlag(false),
    m_radioConfigurationDataArray(DefaultRadioConfigurationDataArray)
{
}


bool ZetaRF::begin(uint8_t channel, uint8_t packetLength)
{
    m_channelNumber = channel;

    if (packetLength > 0)
        m_packetLength = packetLength;

    SPI.begin();

    pinMode(m_csPin, OUTPUT);
    pinMode(m_irqPin, INPUT_PULLUP);
    pinMode(m_sdnPin, OUTPUT);

    // Power Up the radio chip
    powerUp();

    int retryCount = 10;
    // Load radio configuration
    while (initialize(m_radioConfigurationDataArray) != Success && (retryCount--)) {
        // Wait and retry
        delay(20);
        powerUp();
    }

    // Read ITs, clear pending ones
    readInterruptStatus(0, 0, 0);

    if (!retryCount) return false;
    return true;
}


/*!
 * Set Radio to TX mode, fixed packet length.
 * Uses internal channel number set in @begin().
 *
 * @param data Pointer to data to send.
 */
void ZetaRF::sendPacket(const uint8_t *data)
{
    sendPacket(m_channelNumber, data);
}

/*!
 * Set Radio to TX mode, fixed packet length.
 *
 * @param channel Channel to send data to.
 * @param data Pointer to data to send.
 */
void ZetaRF::sendPacket(uint8_t channel, const uint8_t *data)
{
    if (!data) return;
    // Read ITs, clear pending ones
    readInterruptStatus(0, 0, 0);

    // Fill the TX fifo with data
    writeTxFifo(data, m_packetLength);

    // Start sending packet on channel, START immediately, Packet according to PH
    startTx(channel, 0x30, m_packetLength);
}


/*!
 * Set Radio to RX mode, fixed packet length.
 * Uses internal channel number set in @begin().
 */
void ZetaRF::startReceiver()
{
    startReceiver(m_channelNumber);
}

/*!
 * Set Radio to RX mode, fixed packet length.
 *
 * @param channel Channel to receive data from.
 */
void ZetaRF::startReceiver(uint8_t channel)
{
    // Read ITs, clear pending ones
    readInterruptStatus(0, 0, 0);

    // Start Receiving packet on channel, START immediately, Packet n bytes long
    startRx(channel, 0, m_packetLength,
            SI4455_CMD_START_RX_ARG_RXTIMEOUT_STATE_ENUM_RX,
            SI4455_CMD_START_RX_ARG_RXVALID_STATE_ENUM_RX,
            SI4455_CMD_START_RX_ARG_RXINVALID_STATE_ENUM_RX);
}


/*!
 * Checks if the last transmission succeed.
 */
bool ZetaRF::checkTransmitted()
{
    //if (irqLevel() == false) {
        readInterruptStatus(0, 0, 0);

        if (m_dataTransmittedFlag) {
            m_dataTransmittedFlag = false;
            return true;
        }
    //}
    return false;
}

/*!
 * Checks if an incomping message was received.
 */
bool ZetaRF::checkReceived()
{
    //if (irqLevel() == false) {
        readInterruptStatus(0, 0, 0);

        if (m_dataAvailableFlag) {
            m_dataAvailableFlag = false;
            return true;
        }
    //}
    return false;
}

/*!
 * Read packet from Rx FIFO.
 * @a data must point to a valid array with a size of at least the packet length.
 */
uint8_t ZetaRF::readPacket(uint8_t *data)
{
    if (!data) return 0;

    // Read FIFO info to known how many bytes are pending
    Si4455_FifoInfo &fi = readFifoInfo(0);

    const bool dataRemaining = (fi.RX_FIFO_COUNT > m_packetLength);

    // Read FIFO
    readRxFifo(data, m_packetLength);

    if (dataRemaining) {
        m_dataAvailableFlag = true;
    }

    return m_packetLength;
}


bool ZetaRF::isTxFifoAlmostEmpty()
{
    //if (irqLevel() == false) {
        readInterruptStatus(0, 0, 0);

        if (m_txFifoAlmostEmptyFlag) {
            m_txFifoAlmostEmptyFlag = false;
            return true;
        }
    //}
    return false;
}

bool ZetaRF::isRxFifoAlmostFull()
{
    //if (irqLevel() == false) {
        readInterruptStatus(0, 0, 0);
        
        if (m_rxFifoAlmostFullFlag) {
            m_rxFifoAlmostFullFlag = false;
            return true;
        }
    //}
    return false;
}



/*!
 *  Power up the chip.
 */
void ZetaRF::powerUp()
{
  // Hardware reset the chip
  reset();

  // Wait until reset timeout or Reset IT signal
  //for (unsigned int wDelay = 0; wDelay < RadioConfiguration.Radio_Delay_Cnt_After_Reset; wDelay++);
  delay(100);
}

/*!
 * Hardware reset the chip using shutdown input
 */
void ZetaRF::reset()
{
    // Put radio in shutdown, wait then release
    assertShutdown();
    delay(10);
    deassertShutdown();
    delay(10);
    clearCts();
}

/*!
 * Load all properties and commands with a list of NULL terminated commands.
 * Call @reset before.
 */
ZetaRF::CommandResult ZetaRF::initialize(const uint8_t* configArray)
{
    // While cycle as far as the pointer points to a command
    while (*configArray != 0x00) {
        // Commands structure in the array:
        // --------------------------------
        // LEN | <LEN length of data>

        uint8_t cmdBytesCount = *configArray++;

        if (cmdBytesCount > 16u) {
            // Initial configuration of Si4x55
            if (*configArray == SI4455_CMD_ID_WRITE_TX_FIFO) {
                if (cmdBytesCount > 128u) {
                    // Number of command bytes exceeds maximal allowable length
                    // @todo May need to send NOP to send more than 128 bytes (check documentation)
                    return CommandError;
                }

                // Load array to the device
                configArray++;
                writeEZConfigArray(configArray, cmdBytesCount - 1);

                // Point to the next command
                configArray += cmdBytesCount - 1;

                // Continue command interpreter
                continue;
            } else {
                // Number of command bytes exceeds maximal allowable length
                return CommandError;
            }
        }

        uint8_t radioCmd[16];
        for (uint8_t col = 0; col < cmdBytesCount; col++) {
            radioCmd[col] = *configArray;
            configArray++;
        }

        uint8_t response = 0;
        if (sendCommandAndGetResponse(radioCmd, cmdBytesCount, &response, 1) != 0xFF) {
            // Timeout occured
            return CtsTimeout;
        }

        // Check response byte of EZCONFIG_CHECK command
        if (radioCmd[0] == SI4455_CMD_ID_EZCONFIG_CHECK) {
            if (response) {
                // Number of command bytes exceeds maximal allowable length
                return CommandError;
            }    
        }

        if (!irqLevel()) {
            // Get and clear all interrupts.  An error has occured...
            Si4455_InterruptStatus &it = readInterruptStatus(0, 0, 0);
            if (it.CHIP_PEND & SI4455_CMD_GET_CHIP_STATUS_REP_CMD_ERROR_PEND_MASK) {
                return CommandError;
            }
        }
    }

    return Success;
}

/*!
 * Writes data byte(s) to the EZConfig array (array generated from EZConfig tool).
 */
void ZetaRF::writeEZConfigArray(const uint8_t* ezConfigArray, uint8_t count)
{
    writeData(SI4455_CMD_ID_WRITE_TX_FIFO, ezConfigArray, count, true);
}

/*!
 * Validates the EZConfig array was written correctly.
 */
uint8_t ZetaRF::checkEZConfig(uint16_t checksum)
{
    // Do not check CTS
    clearCS();

    // Command byte
    spiWriteByte(SI4455_CMD_ID_EZCONFIG_CHECK);

    // CRC
    spiWriteByte((uint16_t) checksum >> 8u);
    spiWriteByte((uint16_t) checksum & 0x00FF);

    setCS();

    // Get the response from the radio chip
    uint8_t data = 0;
    getResponse(&data, 1);

    return data;
}

/*!
 * Switches to TX state and starts transmission of a packet.
 */
void ZetaRF::startTx(uint8_t channel, uint8_t condition, uint16_t length)
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_START_TX,
        channel,
        condition,
        (uint8_t)(length >> 8),
        (uint8_t)(length)
    };

    sendCommand(buffer, SI4455_CMD_ARG_COUNT_START_TX);
}

/*!
 * Writes data byte(s) to the TX FIFO.
 */
void ZetaRF::writeTxFifo(const uint8_t* data, uint8_t length)
{
    writeData(SI4455_CMD_ID_WRITE_TX_FIFO, data, length, false);
}

/*!
 * Returns the interrupt status of the Modem Interrupt Group (both STATUS and PENDING).
 * Optionally, it may be used to clear latched (PENDING) interrupt events.
 */
Si4455_ModemStatus& ZetaRF::readModemStatus(uint8_t clearPendingModem)
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_GET_MODEM_STATUS,
        clearPendingModem
    };

    sendCommandAndGetResponse(buffer, SI4455_CMD_ARG_COUNT_GET_MODEM_STATUS,
                              m_commandReply.RAW, SI4455_CMD_REPLY_COUNT_GET_MODEM_STATUS);
    return m_commandReply.GET_MODEM_STATUS;

    // Si4455Cmd.GET_MODEM_STATUS.MODEM_PEND   = radioCmd[0];
    // Si4455Cmd.GET_MODEM_STATUS.MODEM_STATUS = radioCmd[1];
    // Si4455Cmd.GET_MODEM_STATUS.CURR_RSSI    = radioCmd[2];
    // Si4455Cmd.GET_MODEM_STATUS.LATCH_RSSI   = radioCmd[3];
    // Si4455Cmd.GET_MODEM_STATUS.ANT1_RSSI    = radioCmd[4];
    // Si4455Cmd.GET_MODEM_STATUS.ANT2_RSSI    = radioCmd[5];
    // Si4455Cmd.GET_MODEM_STATUS.AFC_FREQ_OFFSET.U8[MSB]  = radioCmd[6];
    // Si4455Cmd.GET_MODEM_STATUS.AFC_FREQ_OFFSET.U8[LSB]  = radioCmd[7];
}

/*!
 * Switches to RX state and starts reception of a packet.
 */
void ZetaRF::startRx(uint8_t channel, uint8_t condition, uint16_t length, uint8_t nextState1, uint8_t nextState2, uint8_t nextState3)
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_START_RX,
        channel,
        condition,
        (uint8_t)(length >> 8),
        (uint8_t)(length),
        nextState1,
        nextState2,
        nextState3
    };

    sendCommand(buffer, SI4455_CMD_ARG_COUNT_START_RX);
}

/*!
 * Reads data byte(s) from the RX FIFO.
 */
void ZetaRF::readRxFifo(uint8_t* data, uint8_t length)
{
    readData(SI4455_CMD_ID_READ_RX_FIFO, data, length, false);
}

/*!
 * Returns the interrupt status of ALL the possible interrupt events (both STATUS and PENDING).
 * Optionally, it may be used to clear latched (PENDING) interrupt events.
 */
Si4455_InterruptStatus& ZetaRF::readInterruptStatus(uint8_t clearPendingPH, uint8_t clearPendingModem, uint8_t clearPendingChip)
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_GET_INT_STATUS,
        clearPendingPH,
        clearPendingModem,
        clearPendingChip
    };

    sendCommandAndGetResponse(buffer, SI4455_CMD_ARG_COUNT_GET_INT_STATUS,
                              m_commandReply.RAW, SI4455_CMD_REPLY_COUNT_GET_INT_STATUS);

    if (m_commandReply.GET_INT_STATUS.PH_PEND & SI4455_CMD_GET_INT_STATUS_REP_PACKET_SENT_PEND_BIT) {
        m_dataTransmittedFlag = true;
    }
    if (m_commandReply.GET_INT_STATUS.PH_PEND & SI4455_CMD_GET_INT_STATUS_REP_PACKET_RX_PEND_BIT) {
        // @todo Add circular buffer?
        m_dataAvailableFlag = true;
    }

    if (m_commandReply.GET_INT_STATUS.PH_PEND & SI4455_CMD_GET_INT_STATUS_REP_CRC_ERROR_PEND_BIT) {
        m_crcErrorFlag = true;
    }

    if (m_commandReply.GET_INT_STATUS.PH_PEND & SI4455_CMD_GET_INT_STATUS_REP_TX_FIFO_ALMOST_EMPTY_PEND_BIT) {
        m_txFifoAlmostEmptyFlag = true;
    }
    if (m_commandReply.GET_INT_STATUS.PH_PEND & SI4455_CMD_GET_INT_STATUS_REP_RX_FIFO_ALMOST_FULL_PEND_BIT) {
        m_rxFifoAlmostFullFlag = true;
    }

    return m_commandReply.GET_INT_STATUS;

    // m_commandReply.GET_INT_STATUS.INT_PEND       = radioCmd[0];
    // m_commandReply.GET_INT_STATUS.INT_STATUS     = radioCmd[1];
    // m_commandReply.GET_INT_STATUS.PH_PEND        = radioCmd[2];
    // m_commandReply.GET_INT_STATUS.PH_STATUS      = radioCmd[3];
    // m_commandReply.GET_INT_STATUS.MODEM_PEND     = radioCmd[4];
    // m_commandReply.GET_INT_STATUS.MODEM_STATUS   = radioCmd[5];
    // m_commandReply.GET_INT_STATUS.CHIP_PEND      = radioCmd[6];
    // m_commandReply.GET_INT_STATUS.CHIP_STATUS    = radioCmd[7];
}

/*!
 * Configures the GPIO pins.
 */
Si4455_GpioPinConfig& ZetaRF::configureGpioPins(uint8_t gpio0, uint8_t gpio1, uint8_t gpio2, uint8_t gpio3,
                                                uint8_t nirq, uint8_t sdo,
                                                uint8_t genConfig)
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_GPIO_PIN_CFG,
        gpio0,
        gpio1,
        gpio2,
        gpio3,
        nirq,
        sdo,
        genConfig
    };

    sendCommandAndGetResponse(buffer, SI4455_CMD_ARG_COUNT_GPIO_PIN_CFG,
                              m_commandReply.RAW, SI4455_CMD_REPLY_COUNT_GPIO_PIN_CFG);
    return m_commandReply.GPIO_PIN_CFG;

    // m_commandReply.GPIO_PIN_CFG.GPIO0        = radioCmd[0];
    // m_commandReply.GPIO_PIN_CFG.GPIO1        = radioCmd[1];
    // m_commandReply.GPIO_PIN_CFG.GPIO2        = radioCmd[2];
    // m_commandReply.GPIO_PIN_CFG.GPIO3        = radioCmd[3];
    // m_commandReply.GPIO_PIN_CFG.NIRQ         = radioCmd[4];
    // m_commandReply.GPIO_PIN_CFG.SDO          = radioCmd[5];
    // m_commandReply.GPIO_PIN_CFG.GEN_CONFIG   = radioCmd[6];
}


/*!
 * Retrieves the value of one or more properties.
 */
Si4455_Properties& ZetaRF::readProperties(uint8_t group, uint8_t count, uint8_t startProperty)
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_GET_PROPERTY,
        group,
        count,
        startProperty
    };

    sendCommandAndGetResponse(buffer, SI4455_CMD_ARG_COUNT_GET_PROPERTY,
                              m_commandReply.RAW, count);
    return m_commandReply.GET_PROPERTY;

    // Si4455Cmd.GET_PROPERTY.DATA0    = radioCmd[0];
    // Si4455Cmd.GET_PROPERTY.DATA1    = radioCmd[1];
    // Si4455Cmd.GET_PROPERTY.DATA2    = radioCmd[2];
    // Si4455Cmd.GET_PROPERTY.DATA3    = radioCmd[3];
    // Si4455Cmd.GET_PROPERTY.DATA4    = radioCmd[4];
    // Si4455Cmd.GET_PROPERTY.DATA5    = radioCmd[5];
    // Si4455Cmd.GET_PROPERTY.DATA6    = radioCmd[6];
    // Si4455Cmd.GET_PROPERTY.DATA7    = radioCmd[7];
    // Si4455Cmd.GET_PROPERTY.DATA8    = radioCmd[8];
    // Si4455Cmd.GET_PROPERTY.DATA9    = radioCmd[9];
    // Si4455Cmd.GET_PROPERTY.DATA10   = radioCmd[10];
    // Si4455Cmd.GET_PROPERTY.DATA11   = radioCmd[11];
    // Si4455Cmd.GET_PROPERTY.DATA12   = radioCmd[12];
    // Si4455Cmd.GET_PROPERTY.DATA13   = radioCmd[13];
    // Si4455Cmd.GET_PROPERTY.DATA14   = radioCmd[14];
    // Si4455Cmd.GET_PROPERTY.DATA15   = radioCmd[15];
}

/*!
 * Sets the value of one or more properties.
 */
void ZetaRF::setProperties(uint8_t group, uint8_t count, uint8_t property, ...)
{
    va_list argList;
    uint8_t cmdIndex;

    uint8_t buffer[16] = {    // No more than 12 properties allowed
        SI4455_CMD_ID_SET_PROPERTY,
        group,
        count,
        property
    };

    va_start(argList, property);
    cmdIndex = 4;
    while (count--) {
        buffer[cmdIndex] = (uint8_t)(va_arg(argList, int));
        cmdIndex++;
        if (cmdIndex == 15) break;
    }
    va_end(argList);

    sendCommand(buffer, cmdIndex);
}

/*!
 * Request current device state and channel.
 */
Si4455_DeviceState& ZetaRF::requestDeviceState()
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_REQUEST_DEVICE_STATE
    };

    sendCommandAndGetResponse(buffer, SI4455_CMD_ARG_COUNT_REQUEST_DEVICE_STATE,
                              m_commandReply.RAW, SI4455_CMD_REPLY_COUNT_REQUEST_DEVICE_STATE);
    return m_commandReply.REQUEST_DEVICE_STATE;

    // Si4455Cmd.REQUEST_DEVICE_STATE.CURR_STATE       = radioCmd[0];
    // Si4455Cmd.REQUEST_DEVICE_STATE.CURRENT_CHANNEL  = radioCmd[1];
}

/*!
 * Manually switch the chip to a desired operating state.
 */
void ZetaRF::changeState(uint8_t nextState1)
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_CHANGE_STATE,
        nextState1
    };

    sendCommand(buffer, SI4455_CMD_ARG_COUNT_CHANGE_STATE);
}


/*!
 * No Operation command.
 */
void ZetaRF::nopCommand()
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_NOP
    };

    sendCommand(buffer, SI4455_CMD_ARG_COUNT_NOP);
}


/*!
 * Access the current byte counts in the TX and RX FIFOs, and provide for resetting the FIFOs.
 */
Si4455_FifoInfo& ZetaRF::readFifoInfo(uint8_t fifo)
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_FIFO_INFO,
        fifo
    };

    sendCommandAndGetResponse(buffer, SI4455_CMD_ARG_COUNT_FIFO_INFO,
                              m_commandReply.RAW, SI4455_CMD_REPLY_COUNT_FIFO_INFO);
    return m_commandReply.FIFO_INFO;

    // Si4455Cmd.FIFO_INFO.RX_FIFO_COUNT   = radioCmd[0];
    // Si4455Cmd.FIFO_INFO.TX_FIFO_SPACE   = radioCmd[1];
}

/*!
 * Reports basic information about the device.
 */
const Si4455_PartInfo& ZetaRF::readPartInfo()
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_PART_INFO
    };

    sendCommandAndGetResponse(buffer, SI4455_CMD_ARG_COUNT_PART_INFO,
                              m_commandReply.RAW, SI4455_CMD_REPLY_COUNT_PART_INFO);
    return m_commandReply.PART_INFO;

    // Si4455Cmd.PART_INFO.CHIPREV         = radioCmd[0];
    // Si4455Cmd.PART_INFO.PART.U8[MSB]    = radioCmd[1];
    // Si4455Cmd.PART_INFO.PART.U8[LSB]    = radioCmd[2];
    // Si4455Cmd.PART_INFO.PBUILD          = radioCmd[3];
    // Si4455Cmd.PART_INFO.ID.U8[MSB]      = radioCmd[4];
    // Si4455Cmd.PART_INFO.ID.U8[LSB]      = radioCmd[5];
    // Si4455Cmd.PART_INFO.CUSTOMER        = radioCmd[6];
    // Si4455Cmd.PART_INFO.ROMID           = radioCmd[7];
    // Si4455Cmd.PART_INFO.BOND            = radioCmd[8];
}

/*!
 * Returns the Function revision information of the device.
 */
const Si4455_FuncInfo& ZetaRF::readFuncInfo()
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_FUNC_INFO
    };

    sendCommandAndGetResponse(buffer, SI4455_CMD_ARG_COUNT_FUNC_INFO,
                              m_commandReply.RAW, SI4455_CMD_REPLY_COUNT_FUNC_INFO);
    return m_commandReply.FUNC_INFO;

    // Si4455Cmd.FUNC_INFO.REVEXT          = radioCmd[0];
    // Si4455Cmd.FUNC_INFO.REVBRANCH       = radioCmd[1];
    // Si4455Cmd.FUNC_INFO.REVINT          = radioCmd[2];
    // Si4455Cmd.FUNC_INFO.PATCH.U8[MSB]   = radioCmd[3];
    // Si4455Cmd.FUNC_INFO.PATCH.U8[LSB]   = radioCmd[4];
    // Si4455Cmd.FUNC_INFO.FUNC            = radioCmd[5];
    // Si4455Cmd.FUNC_INFO.SVNFLAGS        = radioCmd[6];
    // Si4455Cmd.FUNC_INFO.SVNREV.U8[b3]   = radioCmd[7];
    // Si4455Cmd.FUNC_INFO.SVNREV.U8[b2]   = radioCmd[8];
    // Si4455Cmd.FUNC_INFO.SVNREV.U8[b1]   = radioCmd[9];
    // Si4455Cmd.FUNC_INFO.SVNREV.U8[b0]   = radioCmd[10];
}


/*!
 * Reads the fast response registers (FRR) starting with FRR_A.
 */
Si4455_FrrA& ZetaRF::readFrrA(uint8_t count)
{
    readData(SI4455_CMD_ID_FRR_A_READ,
             m_commandReply.RAW,
             count,
             false);
    return m_commandReply.FRR_A_READ;

    // Si4455Cmd.FRR_A_READ.FRR_A_VALUE = radioCmd[0];
    // Si4455Cmd.FRR_A_READ.FRR_B_VALUE = radioCmd[1];
    // Si4455Cmd.FRR_A_READ.FRR_C_VALUE = radioCmd[2];
    // Si4455Cmd.FRR_A_READ.FRR_D_VALUE = radioCmd[3];
}

/*!
 * Reads the fast response registers (FRR) starting with FRR_B.
 */
Si4455_FrrB& ZetaRF::readFrrB(uint8_t count)
{
    readData(SI4455_CMD_ID_FRR_B_READ,
             m_commandReply.RAW,
             count,
             false);
    return m_commandReply.FRR_B_READ;

    // Si4455Cmd.FRR_B_READ.FRR_B_VALUE = radioCmd[0];
    // Si4455Cmd.FRR_B_READ.FRR_C_VALUE = radioCmd[1];
    // Si4455Cmd.FRR_B_READ.FRR_D_VALUE = radioCmd[2];
    // Si4455Cmd.FRR_B_READ.FRR_A_VALUE = radioCmd[3];
}

/*!
 * Reads the fast response registers (FRR) starting with FRR_C.
 */
Si4455_FrrC& ZetaRF::readFrrC(uint8_t count)
{
    readData(SI4455_CMD_ID_FRR_C_READ,
             m_commandReply.RAW,
             count,
             false);
    return m_commandReply.FRR_C_READ;

    // Si4455Cmd.FRR_C_READ.FRR_C_VALUE = radioCmd[0];
    // Si4455Cmd.FRR_C_READ.FRR_D_VALUE = radioCmd[1];
    // Si4455Cmd.FRR_C_READ.FRR_A_VALUE = radioCmd[2];
    // Si4455Cmd.FRR_C_READ.FRR_B_VALUE = radioCmd[3];
}

/*!
 * Reads the fast response registers (FRR) starting with FRR_D.
 */
Si4455_FrrD& ZetaRF::readFrrD(uint8_t count)
{
    readData(SI4455_CMD_ID_FRR_D_READ,
             m_commandReply.RAW,
             count,
             false);
    return m_commandReply.FRR_D_READ;

    // Si4455Cmd.FRR_D_READ.FRR_D_VALUE = radioCmd[0];
    // Si4455Cmd.FRR_D_READ.FRR_A_VALUE = radioCmd[1];
    // Si4455Cmd.FRR_D_READ.FRR_B_VALUE = radioCmd[2];
    // Si4455Cmd.FRR_D_READ.FRR_C_VALUE = radioCmd[3];
}


/*!
 * Used to read CTS and the command response.
 */
Si4455_CommandBuffer& ZetaRF::readCommandBuffer()
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_READ_CMD_BUFF
    };

    sendCommandAndGetResponse(buffer, SI4455_CMD_ARG_COUNT_READ_CMD_BUFF,
                              m_commandReply.RAW, SI4455_CMD_REPLY_COUNT_READ_CMD_BUFF);
    return m_commandReply.READ_CMD_BUFF;

    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF0   = radioCmd[0];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF1   = radioCmd[1];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF2   = radioCmd[2];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF3   = radioCmd[3];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF4   = radioCmd[4];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF5   = radioCmd[5];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF6   = radioCmd[6];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF7   = radioCmd[7];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF8   = radioCmd[8];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF9   = radioCmd[9];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF10  = radioCmd[10];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF11  = radioCmd[11];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF12  = radioCmd[12];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF13  = radioCmd[13];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF14  = radioCmd[14];
    // Si4455Cmd.READ_CMD_BUFF.CMD_BUFF15  = radioCmd[15];
}

/*!
 * Performs conversions using the Auxiliary ADC and returns the results of those conversions.
 */
Si4455_AdcReadings& ZetaRF::readADC(uint8_t adcEnable, uint8_t adcConfig)
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_GET_ADC_READING,
        adcEnable,
        adcConfig
    };

    sendCommandAndGetResponse(buffer, SI4455_CMD_ARG_COUNT_GET_ADC_READING,
                              m_commandReply.RAW, SI4455_CMD_REPLY_COUNT_GET_ADC_READING);
    return m_commandReply.GET_ADC_READING;

    // Si4455Cmd.GET_ADC_READING.GPIO_ADC.U8[MSB]      = radioCmd[0];
    // Si4455Cmd.GET_ADC_READING.GPIO_ADC.U8[LSB]      = radioCmd[1];
    // Si4455Cmd.GET_ADC_READING.BATTERY_ADC.U8[MSB]   = radioCmd[2];
    // Si4455Cmd.GET_ADC_READING.BATTERY_ADC.U8[LSB]   = radioCmd[3];
    // Si4455Cmd.GET_ADC_READING.TEMP_ADC.U8[MSB]      = radioCmd[4];
    // Si4455Cmd.GET_ADC_READING.TEMP_ADC.U8[LSB]      = radioCmd[5];
    // Si4455Cmd.GET_ADC_READING.TEMP_SLOPE            = radioCmd[6];
    // Si4455Cmd.GET_ADC_READING.TEMP_INTERCEPT        = radioCmd[7];
}

/*!
 * Returns the interrupt status of the Packet Handler Interrupt Group (both STATUS and PENDING).
 * Optionally, it may be used to clear latched (PENDING) interrupt events.
 */
Si4455_PhStatus& ZetaRF::readPhStatus(uint8_t clearPendingPH)
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_GET_PH_STATUS,
        clearPendingPH
    };

    sendCommandAndGetResponse(buffer, SI4455_CMD_ARG_COUNT_GET_PH_STATUS,
                              m_commandReply.RAW, SI4455_CMD_REPLY_COUNT_GET_PH_STATUS);
    return m_commandReply.GET_PH_STATUS;

    // Si4455Cmd.GET_PH_STATUS.PH_PEND        = radioCmd[0];
    // Si4455Cmd.GET_PH_STATUS.PH_STATUS      = radioCmd[1];
}

/*!
 * Returns the interrupt status of the Chip Interrupt Group (both STATUS and PENDING).
 * Optionally, it may be used to clear latched (PENDING) interrupt events.
 */
Si4455_ChipStatus& ZetaRF::readChipStatus(uint8_t clearPendingChip)
{
    const uint8_t buffer[] = {
        SI4455_CMD_ID_GET_CHIP_STATUS,
        clearPendingChip
    };

    sendCommandAndGetResponse(buffer, SI4455_CMD_ARG_COUNT_GET_CHIP_STATUS,
                              m_commandReply.RAW, SI4455_CMD_REPLY_COUNT_GET_CHIP_STATUS);
    return m_commandReply.GET_CHIP_STATUS;

    // Si4455Cmd.GET_CHIP_STATUS.CHIP_PEND         = radioCmd[0];
    // Si4455Cmd.GET_CHIP_STATUS.CHIP_STATUS       = radioCmd[1];
    // Si4455Cmd.GET_CHIP_STATUS.CMD_ERR_STATUS    = radioCmd[2];
}



/*!
 * Gets a command response from the radio chip.
 *
 * @param data  Pointer to where to put the data.
 * @param count Number of bytes to get from the radio chip.
 *
 * @return CTS value.
 */
uint8_t ZetaRF::getResponse(uint8_t* data, uint8_t count)
{
    uint8_t ctsVal = 0;
    uint16_t errorCount = RADIO_CTS_TIMEOUT;

    while (errorCount != 0) {   // Wait until radio IC is ready with the data
        clearCS();
        spiWriteByte(0x44);     // Read CMD buffer
        ctsVal = spiReadByte();

        if (ctsVal == 0xFF) {
            if (count) {
                spiReadData(data, count);
            }
            setCS();
            break;
        }

        setCS();
        errorCount--;
    }

    if (errorCount == 0) {
        // ERROR! Should never take this long
        // @todo Error callback ?
        return 0;
    }

    if (ctsVal == 0xFF) {
        m_ctsWentHigh = true;
    }

    return ctsVal;
}

/*!
 * Sends a command to the radio chip.
 *
 * @param data  Pointer to the command to send.
 * @param count Number of bytes in the command to send to the radio device.
 */
void ZetaRF::sendCommand(const uint8_t* data, uint8_t count)
{
    while (!m_ctsWentHigh) {
        pollCts();
    }

    clearCS();
    spiWriteData(data, count);
    setCS();

    clearCts();
}

/*!
 * Sends a command to the radio chip and gets a response.
 *
 * @param commandData       Pointer to the command data.
 * @param commandByteCount  Number of bytes in the command to send to the radio device.
 * @param responseData      Pointer to where to put the response data.
 * @param responseByteCount Number of bytes in the response to fetch.
 *
 * @return CTS value.
 */
uint8_t ZetaRF::sendCommandAndGetResponse(const uint8_t* commandData, uint8_t commandByteCount, uint8_t* responseData, uint8_t responseByteCount)
{
    sendCommand(commandData, commandByteCount);
    return getResponse(responseData, responseByteCount);
}


/*!
 * Gets a command response from the radio chip.
 *
 * @param command     Command ID.
 * @param data        Pointer to where to put the data.
 * @param count       Number of bytes to get from the radio chip.
 * @param pollCtsFlag Set to poll CTS.
 */
void ZetaRF::readData(uint8_t command, uint8_t* data, uint8_t count, bool pollCtsFlag)
{
    if (pollCtsFlag) {
        while (!m_ctsWentHigh) {
            pollCts();
        }
    }

    clearCS();
    spiWriteByte(command);
    spiReadData(data, count);
    setCS();

    clearCts();
}

/*!
 * Gets a command response from the radio chip.
 *
 * @param command     Command ID.
 * @param data        Pointer to where to put the data.
 * @param count       Number of bytes to get from the radio chip.
 * @param pollCtsFlag Set to poll CTS.
 */
void ZetaRF::writeData(uint8_t command, const uint8_t* data, uint8_t count, bool pollCtsFlag)
{
    if (pollCtsFlag) {
        while (!m_ctsWentHigh) {
            pollCts();
        }
    }

    clearCS();
    spiWriteByte(command);
    spiWriteData(data, count);
    setCS();

    clearCts();
}


/*!
 * Waits for CTS to be high.
 *
 * @return CTS value.
 */
uint8_t ZetaRF::pollCts()
{
#ifdef RADIO_USER_CFG_USE_GPIO1_FOR_CTS
    while (!gpio1Level()) {
        // Wait...
    }

    m_ctsWentHigh = true;
    return 0xFF;
#else
    return getResponse(NULL, 0);
#endif
}

/*!
 * Clears the CTS state variable.
 */
void ZetaRF::clearCts()
{
    m_ctsWentHigh = false;
}



void ZetaRF::assertShutdown() const
{
    digitalWrite(m_sdnPin, HIGH);
}
void ZetaRF::deassertShutdown() const
{
    digitalWrite(m_sdnPin, LOW);
}

void ZetaRF::clearCS() const
{
    SPI.beginTransaction(ZETARF_SPI_SETTINGS);
    digitalWrite(m_csPin, LOW);
}
void ZetaRF::setCS() const
{
    digitalWrite(m_csPin, HIGH);
    SPI.endTransaction();
}

bool ZetaRF::irqLevel() const
{
    return (digitalRead(m_irqPin) == HIGH);
}


uint8_t ZetaRF::spiReadWriteByte(uint8_t value) const
{
    return SPI.transfer(value);
}

void ZetaRF::spiWriteByte(uint8_t value) const
{
    spiReadWriteByte(value);
}
uint8_t ZetaRF::spiReadByte() const
{
    return spiReadWriteByte(0xFF);
}


void ZetaRF::spiReadWriteData(uint8_t* data, uint8_t count) const
{
    SPI.transfer(data, count);
}

void ZetaRF::spiWriteData(const uint8_t* data, uint8_t count) const
{
    while (count--) {
        SPI.transfer(*data++);
    }
}
void ZetaRF::spiReadData(uint8_t* data, uint8_t count) const
{
    while (count--) {
        *data++ = SPI.transfer(0xFF);
    }
}

