#include "dbc_classes.h"
#include "dbchandler.h"
#include "utility.h"

DBC_MESSAGE::DBC_MESSAGE()
{
    sigHandler = new DBCSignalHandler;
}

/*
 The way that the DBC file format works is kind of weird... For intel format signals you count up
from the start bit to the end bit which is (startbit + signallength - 1). At each point
bits are numbered in a sawtooth manner. What that means is that the very first bit is 0 and you count up
from there all of the way to 63 with each byte being 8 bits so bit 0 is the lowest bit in the first byte
and 8 is the lowest bit in the next byte up. The whole thing looks like this:
                 Bits
      7  6  5  4  3  2  1  0

  0   7  6  5  4  3  2  1  0
b 1   15 14 13 12 11 10 9  8
y 2   23 22 21 20 19 18 17 16
t 3   31 30 29 28 27 26 25 24
e 4   39 38 37 36 35 34 33 32
s 5   47 46 45 44 43 42 41 40
  6   55 54 53 52 51 50 49 48
  7   63 62 61 60 59 58 57 56

  For intel format you start at the start bit and keep counting up. If you have a signal size of 8
  and start at bit 12 then the bits are 12, 13, 14, 15, 16, 17, 18, 19 which spans across two bytes.
  In this format each bit is worth twice as much as the last and you just keep counting up.
  Bit 12 is worth 1, 13 is worth 2, 14 is worth 4, etc all of the way to bit 19 is worth 128.

  Motorola format turns most everything on its head. You count backward from the start bit but
  only within the current byte. If you are about to exit the current byte you go one higher and then keep
  going backward as before. Using the same example as for intel, start bit of 12 and a signal length of 8.
  So, the bits are 12, 11, 10, 9, 8, 23, 22, 21. Yes, that's confusing. They now go in reverse value order too.
  Bit 12 is worth 128, 11 is worth 64, etc until bit 21 is worth 1.
*/
bool DBC_SIGNAL::processAsText(const CANFrame &frame, QString &outString)
{
    int64_t result = 0;
    bool isSigned = false;
    double endResult;

    if (valType == STRING)
    {
        QString buildString;
        int startByte = startBit / 8;
        int bytes = signalSize / 8;
        for (int x = 0; x < bytes; x++) buildString.append(frame.data[startByte + x]);
        outString = buildString;
        return true;
    }

    //if this is a multiplexed signal then we have to see if it is even found in the current message
    if (isMultiplexed)
    {
        if (parentMessage->multiplexorSignal != NULL)
        {
           int val;
           if (!parentMessage->multiplexorSignal->processAsInt(frame, val)) return false;
           if (val != multiplexValue) return false; //signal not found in this message
        }
        else return false;
    }

    if (valType == SIGNED_INT) isSigned = true;
    if (valType == SIGNED_INT || valType == UNSIGNED_INT)
    {
        result = Utility::processIntegerSignal(frame.data, startBit, signalSize, intelByteOrder, isSigned);
        endResult = ((double)result * factor) + bias;
        result = (int64_t)endResult;
    }
    else if (valType == SP_FLOAT)
    {
        //The theory here is that we force the integer signal code to treat this as
        //a 32 bit unsigned integer. This integer is then cast into a float in such a way
        //that the bytes that make up the integer are instead treated as having made up
        //a 32 bit single precision float. That's evil incarnate but it is very fast and small
        //in terms of new code.
        result = Utility::processIntegerSignal(frame.data, startBit, 32, false, false);
        endResult = (*((float *)(&result)) * factor) + bias;
    }
    else //double precision float
    {
        if ( frame.len < 8 )
        {
            result = 0;
            return false;
        }
        //like the above, this is rotten and evil and wrong in so many ways. Force
        //calculation of a 64 bit integer and then cast it into a double.
        result = Utility::processIntegerSignal(frame.data, 0, 64, false, false);
        endResult = (*((double *)(&result)) * factor) + bias;
    }

    QString outputString;

    outputString = name + ": ";

    if (valList.count() > 0) //if this is a value list type then look it up and display the proper string
    {
        bool foundVal = false;
        for (int x = 0; x < valList.count(); x++)
        {
            if (valList.at(x).value == result)
            {
                outputString += valList.at(x).descript;
                foundVal = true;
                break;
            }
        }
        if (!foundVal) outputString += QString::number(endResult) + unitName;
    }
    else //otherwise display the actual number and unit (if it exists)
    {
       outputString += QString::number(endResult) + unitName;
    }

    outString = outputString;
    return true;
}

//Works quite a bit like the above version but this one is cut down and only will return int32_t which is perfect for
//uses like calculating a multiplexor value or if you know you are going to get an integer returned
//from a signal and you want to use it as-is and not have to convert back from a string. Use with caution though
//as this basically assumes the signal is an integer.
//The call syntax is different from the more generic processSignal. Instead of returning the value we return
//true or false to show whether the function succeeded. The variable to fill out is passed by reference.
bool DBC_SIGNAL::processAsInt(const CANFrame &frame, int32_t &outValue)
{
    int32_t result = 0;
    bool isSigned = false;
    if (valType == STRING || valType == SP_FLOAT  || valType == DP_FLOAT)
    {
        return false;
    }

    //if this is a multiplexed signal then we have to see if it is even found in the current message
    if (isMultiplexed)
    {
        if (parentMessage->multiplexorSignal != NULL)
        {
           int val;
           if (!parentMessage->multiplexorSignal->processAsInt(frame, val)) return false;
           if (val != multiplexValue) return false; //signal not found in this message
        }
        else return false;
    }

    if (valType == SIGNED_INT) isSigned = true;
    if ( frame.len*8 < (startBit+signalSize) )
    {
        result = 0;
        return false;
    }
    result = Utility::processIntegerSignal(frame.data, startBit, signalSize, intelByteOrder, isSigned);

    double endResult = ((double)result * factor) + bias;
    result = (int32_t)endResult;

    outValue = result;
    return true;
}

//Another cut down version that will only return double precision data. This can be used on any of the types
//except STRING. Useful for when you know you'll need floating point data and don't want to incur a conversion
//back and forth to double or float. Such a use is the graphing window.
//Similar syntax to processSignalInt but with double instead.
bool DBC_SIGNAL::processAsDouble(const CANFrame &frame, double &outValue)
{
    int64_t result = 0;
    bool isSigned = false;
    double endResult;

    if (valType == STRING)
    {
        return false;
    }

    //if this is a multiplexed signal then we have to see if it is even found in the current message
    if (isMultiplexed)
    {
        if (parentMessage->multiplexorSignal != NULL)
        {
           int val;
           if (!parentMessage->multiplexorSignal->processAsInt(frame, val)) return false;
           if (val != multiplexValue) return false; //signal not found in this message
        }
        else return false;
    }

    if (valType == SIGNED_INT) isSigned = true;
    if (valType == SIGNED_INT || valType == UNSIGNED_INT)
    {
        if ( frame.len*8 < (startBit+signalSize) )
        {
            result = 0;
            return false;
        }
        result = Utility::processIntegerSignal(frame.data, startBit, signalSize, intelByteOrder, isSigned);
        endResult = ((double)result * factor) + bias;
        result = (int64_t)endResult;
    }
    /*TODO: It should be noted that the below floating point has not even been tested. For shame! Test it!*/
    else if (valType == SP_FLOAT)
    {
        if ( frame.len*8 < (startBit+32) )
        {
            result = 0;
            return false;
        }
        //The theory here is that we force the integer signal code to treat this as
        //a 32 bit unsigned integer. This integer is then cast into a float in such a way
        //that the bytes that make up the integer are instead treated as having made up
        //a 32 bit single precision float. That's evil incarnate but it is very fast and small
        //in terms of new code.
        result = Utility::processIntegerSignal(frame.data, startBit, 32, false, false);
        endResult = (*((float *)(&result)) * factor) + bias;
    }
    else //double precision float
    {
        if ( frame.len < 8 )
        {
            result = 0;
            return false;
        }
        //like the above, this is rotten and evil and wrong in so many ways. Force
        //calculation of a 64 bit integer and then cast it into a double.
        result = Utility::processIntegerSignal(frame.data, 0, 64, false, false);
        endResult = (*((double *)(&result)) * factor) + bias;
    }

    outValue = endResult;
    return true;
}

DBC_ATTRIBUTE_VALUE *DBC_SIGNAL::findAttrValByName(QString name)
{
    if (attributes.length() == 0) return NULL;
    for (int i = 0; i < attributes.length(); i++)
    {
        if (attributes[i].attrName.compare(name, Qt::CaseInsensitive) == 0)
        {
            return &attributes[i];
        }
    }
    return NULL;
}

DBC_ATTRIBUTE_VALUE *DBC_SIGNAL::findAttrValByIdx(int idx)
{
    if (idx < 0) return NULL;
    if (idx >= attributes.count()) return NULL;
    return &attributes[idx];
}

DBC_ATTRIBUTE_VALUE *DBC_MESSAGE::findAttrValByName(QString name)
{
    if (attributes.length() == 0) return NULL;
    for (int i = 0; i < attributes.length(); i++)
    {
        if (attributes[i].attrName.compare(name, Qt::CaseInsensitive) == 0)
        {
            return &attributes[i];
        }
    }
    return NULL;
}

DBC_ATTRIBUTE_VALUE *DBC_MESSAGE::findAttrValByIdx(int idx)
{
    if (idx < 0) return NULL;
    if (idx >= attributes.count()) return NULL;
    return &attributes[idx];
}

DBC_ATTRIBUTE_VALUE *DBC_NODE::findAttrValByName(QString name)
{
    if (attributes.length() == 0) return NULL;
    for (int i = 0; i < attributes.length(); i++)
    {
        if (attributes[i].attrName.compare(name, Qt::CaseInsensitive) == 0)
        {
            return &attributes[i];
        }
    }
    return NULL;
}

DBC_ATTRIBUTE_VALUE *DBC_NODE::findAttrValByIdx(int idx)
{
    if (idx < 0) return NULL;
    if (idx >= attributes.count()) return NULL;
    return &attributes[idx];
}
