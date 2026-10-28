#pragma once
#ifndef MATHOPS_H
#define MATHOPS_H

#include <cstddef>
#include <string>

void InitMathOps(const std::string& selectedFeature);
extern void (*optimizedAdd)(const float*, const float*, float*, size_t);
#endif // MATHOPS_H

