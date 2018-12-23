#pragma once

#include "./PIT/PIT.h"
#include "./TeensyDelay/TeensyDelay.h"
#include "MotorControlBase.h"
#include "Stepper.h"

#include <algorithm>

template <unsigned pulseWidth, unsigned accUpdatePeriod>
class StepControlBase : public MotorControlBase
{
  public:  
    // Blocking movements
    template <typename... Steppers>
    void move(Steppers &... steppers);
  
    template <size_t N>
    void move(Stepper *(&motors)[N]);

    // Non-blocking movements    
    void moveAsync(Stepper &stepper);

    template <typename... Steppers>
    void moveAsync(Stepper &stepper, Steppers &... steppers);

    template <size_t N>
    void moveAsync(Stepper *(&motors)[N]);

    // Misc
    void stopAsync();
    void setCallback(void (*_callback)()) { callback = _callback; }

    // Construction
    StepControlBase() = default;
    StepControlBase(const StepControlBase &) = delete;
    StepControlBase &operator=(const StepControlBase &) = delete;

  protected:
    void pitISR();
    void delayISR(unsigned channel);

    unsigned mCnt = 0;
    void (*callback)() = nullptr;

    void doMove(int N, bool mode = true);
    uint32_t virtual prepareMovement(int32_t currentPos, int32_t targetPos, uint32_t targetSpeed, uint32_t acceleration) = 0;
    uint32_t virtual updateSpeed(int32_t currentPosition) = 0;
    uint32_t virtual initiateStopping(int32_t currentPosition) = 0;
};

// Implementation *************************************************************************************************

template <unsigned p, unsigned u>
void StepControlBase<p, u>::doMove(int N, bool move)
{
    //Calculate Bresenham parameters ----------------------------------------------------------------
    std::sort(motorList, motorList + N, Stepper::cmpDelta); // The motor which does most steps leads the movement, move to top of list
    leadMotor = motorList[0];

    for (int i = 1; i < N; i++)
    {
        motorList[i]->B = 2 * motorList[i]->distance - leadMotor->distance;
    }

    // Calculate acceleration parameters --------------------------------------------------------------
    uint32_t targetSpeed = std::abs((*std::min_element(motorList, motorList + N, Stepper::cmpV))->vMax); // use the lowest max frequency for the move, scale by relSpeed
    uint32_t acceleration = (*std::min_element(motorList, motorList + N, Stepper::cmpAcc))->a;           // use the lowest acceleration for the move
    if (leadMotor->distance == 0 || targetSpeed == 0) return;

    // Start move---------------------------------------------------------------------------------------
    StepTimer.stop();
    StepTimer.setFrequency(prepareMovement(leadMotor->current, leadMotor->target, targetSpeed, acceleration));
    StepTimer.start();

    pitISR();                      // initiate first step immediately (no need to wait for the potentially long first cycle)
    delayISR(accLoopDelayChannel); // implicitely start the accLoop
}

// ISR -----------------------------------------------------------------------------------------------------------
template <unsigned p, unsigned u>
void StepControlBase<p, u>::pitISR()
{
    Stepper **slave = motorList;
    leadMotor->doStep(); // move master motor

    while (*(++slave) != nullptr) // move slave motors if required (https://en.wikipedia.org/wiki/Bresenham)
    {
        if ((*slave)->B >= 0)
        {
            (*slave)->doStep();
            (*slave)->B -= leadMotor->distance;
        }
        (*slave)->B += (*slave)->distance;
    }
    TeensyDelay::trigger(p, pinResetDelayChannel); // start delay line to dactivate all step pins

    if (leadMotor->current == leadMotor->target) // stop timer and call callback if we reached target
    {
        StepTimer.stop();
        if (callback != nullptr)
            callback();
    }
}

template <unsigned p, unsigned u>
void StepControlBase<p, u>::delayISR(unsigned channel)
{
    //clear all step pins ----------------------------------------------
    if (channel == pinResetDelayChannel)
    {
        Stepper **motor = motorList;
        while ((*motor) != nullptr)
        {
            (*motor++)->clearStepPin();
        }
    }

    // calculate new speed  --------------------------------------------
    if (channel == accLoopDelayChannel)
    {
        if (StepTimer.isRunning())
        {
            noInterrupts();
            TeensyDelay::trigger(u, accLoopDelayChannel); // retrigger
            interrupts();

            StepTimer.setFrequency(updateSpeed(leadMotor->current));
        }
    }
}

// MOVE ASYNC Commands -------------------------------------------------------------------------------------------------

template <unsigned p, unsigned u>
void StepControlBase<p, u>::moveAsync(Stepper &stepper)
{
    motorList[mCnt++] = &stepper;
    motorList[mCnt] = nullptr;
    doMove(mCnt);
    mCnt = 0;
}

template <unsigned p, unsigned u>
template <typename... Steppers>
void StepControlBase<p, u>::moveAsync(Stepper &stepper, Steppers &... steppers)
{
    static_assert(sizeof...(steppers) < MaxMotors, "Too many motors used. Please increase MaxMotors in file MotorControlBase.h");

    motorList[mCnt++] = &stepper;
    moveAsync(steppers...);
}

template <unsigned p, unsigned u>
template <size_t N>
void StepControlBase<p, u>::moveAsync(Stepper *(&motors)[N]) //move up to maxMotors motors synchronously
{
    static_assert((N + 1) <= sizeof(motorList) / sizeof(motorList[0]), "Too many motors used. Please increase MaxMotors in file MotorControlBase.h");

    for (unsigned i = 0; i < N; i++)
    {
        motorList[i] = motors[i];
    }
    motorList[N] = nullptr;
    doMove(N);
}

// MOVE Commands -------------------------------------------------------------------------------------------------

template <unsigned p, unsigned u>
template <typename... Steppers>
void StepControlBase<p, u>::move(Steppers &... steppers)
{
    moveAsync(steppers...);
    while (isRunning())
    {
        delay(1);
    }
}

template <unsigned p, unsigned u>
template <size_t N>
void StepControlBase<p, u>::move(Stepper *(&motors)[N])
{
    moveAsync(motors, N);
    while (isRunning())
        delay(1);
}

template <unsigned p, unsigned u>
void StepControlBase<p, u>::stopAsync()
{
    uint32_t newTarget = initiateStopping(leadMotor->current);
    leadMotor->target = newTarget;
}