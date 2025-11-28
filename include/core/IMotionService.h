#pragma once

#include "../MotionController.h"

class IMotionService {
public:
    virtual ~IMotionService() = default;

    virtual MotionResult FaultResetAll() = 0;
    virtual MotionResult PowerOnAllAsync(int slaveCount) = 0;
};
