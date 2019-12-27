/* Copyright 2013-2019 Homegear GmbH
 *
 * Homegear is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Homegear is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Homegear.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU Lesser General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
*/

#include "MaxCulfw.h"
#include "../Gd.h"

MaxCulfw::MaxCulfw(BaseLib::SharedObjects* bl) : ICommunicationInterface(bl)
{
    try
    {
        _familyId = MAX_COC_FAMILY_ID;

        _updateMode = false;

        _localRpcMethods.emplace("sendPacket", std::bind(&MaxCulfw::sendPacket, this, std::placeholders::_1));

        _gpio.reset(new BaseLib::LowLevel::Gpio(bl, Gd::settings.gpioPath()));

        start();
    }
    catch(const std::exception& ex)
    {
        Gd::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
}

MaxCulfw::~MaxCulfw()
{
    try
    {
        stop();
    }
    catch(const std::exception& ex)
    {
        Gd::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
}

void MaxCulfw::start()
{
    try
    {
        if(Gd::settings.device().empty())
        {
            Gd::out.printError("Error: No device defined for family MAX! CUL. Please specify it in \"gateway.conf\".");
            return;
        }

        _serial.reset(new BaseLib::SerialReaderWriter(_bl, Gd::settings.device(), 38400, 0, true, 45));
        _eventHandlerSelf = _serial->addEventHandler(this);
        _serial->openDevice(false, false, true);
        if(!_serial->isOpen())
        {
            Gd::out.printError("Error: Could not open device.");
            return;
        }

        if(Gd::settings.gpio2() != -1)
        {
            _gpio->openDevice(Gd::settings.gpio2(), false);
            if(!_gpio->get(Gd::settings.gpio2())) _gpio->set(Gd::settings.gpio2(), true);
            _gpio->closeDevice(Gd::settings.gpio2());
        }
        if(Gd::settings.gpio1() != -1)
        {
            _gpio->openDevice(Gd::settings.gpio1(), false);
            _gpio->set(Gd::settings.gpio1(), false);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            _gpio->set(Gd::settings.gpio1(), true);
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            _gpio->closeDevice(Gd::settings.gpio1());
        }

        std::string packet = "X21\nZr\n";
        _serial->writeLine(packet);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    catch(const std::exception& ex)
    {
        Gd::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
}

void MaxCulfw::stop()
{
    try
    {
        if(!_serial) return;
        _serial->removeEventHandler(_eventHandlerSelf);
        _serial->closeDevice();
        _serial.reset();
    }
    catch(const std::exception& ex)
    {
        Gd::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
}

void MaxCulfw::lineReceived(const std::string& data)
{
    try
    {
        if(data.size() > 21) //21 is minimal packet length (=10 Byte + COC "Z" + "\n")
        {
            std::string packetHex = data.substr(1);
            BaseLib::HelperFunctions::trim(packetHex);
            BaseLib::PArray parameters = std::make_shared<BaseLib::Array>();
            parameters->reserve(2);
            parameters->push_back(std::make_shared<BaseLib::Variable>(MAX_COC_FAMILY_ID));
            parameters->push_back(std::make_shared<BaseLib::Variable>(data));

            if(_invoke)
            {
                auto result = _invoke("packetReceived", parameters);
                if(result->errorStruct && result->structValue->at("faultCode")->integerValue != -1)
                {
                    Gd::out.printError("Error calling packetReceived(): " + result->structValue->at("faultString")->stringValue);
                }
            }
        }
        else if(!data.empty())
        {
            if(data.compare(0, 4, "LOVF") == 0) Gd::out.printWarning("Warning: COC with reached 1% limit. You need to wait, before sending is allowed again.");
            else if(data == "Z") return;
            else Gd::out.printWarning("Warning: Too short packet received: " + data);
        }
    }
    catch(const std::exception& ex)
    {
        Gd::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
}

BaseLib::PVariable MaxCulfw::callMethod(std::string& method, BaseLib::PArray parameters)
{
    try
    {
        auto localMethodIterator = _localRpcMethods.find(method);
        if(localMethodIterator == _localRpcMethods.end()) return BaseLib::Variable::createError(-32601, ": Requested method not found.");

        if(Gd::bl->debugLevel >= 5) Gd::out.printDebug("Debug: Server is calling RPC method: " + method);

        return localMethodIterator->second(parameters);
    }
    catch(const std::exception& ex)
    {
        Gd::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    return BaseLib::Variable::createError(-32500, "Unknown application error. See log for more details.");
}

//{{{ RPC methods
BaseLib::PVariable MaxCulfw::sendPacket(BaseLib::PArray& parameters)
{
    try
    {
        if(parameters->size() != 3 || parameters->at(1)->type != BaseLib::VariableType::tString || parameters->at(1)->stringValue.empty() || parameters->at(2)->type != BaseLib::VariableType::tBoolean) return BaseLib::Variable::createError(-1, "Invalid parameters.");

        if(!_serial)
        {
            Gd::out.printError("Error: Couldn't write to device, because the device descriptor is not valid: " + Gd::settings.device());
            return BaseLib::Variable::createError(-1, "Serial device is not open.");
        }

        std::string packet = "Zs" + parameters->at(1)->stringValue + "\n" + (_updateMode ? "" : "Zr\n");
        _serial->writeLine(packet);

        //Sleep on WOR packet
        if(parameters->at(2)->booleanValue) std::this_thread::sleep_for(std::chrono::milliseconds(1100));

        return std::make_shared<BaseLib::Variable>();
    }
    catch(const std::exception& ex)
    {
        Gd::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    return BaseLib::Variable::createError(-32500, "Unknown application error. See log for more details.");
}
//}}}