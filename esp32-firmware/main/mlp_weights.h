#ifndef MLP_WEIGHTS_H
#define MLP_WEIGHTS_H

static const float W1[3][6] = {
    {1.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {0.0, 1.0, 0.0, 0.0, 0.0, 0.0},
    {0.0, 0.0, 1.0, 0.0, 0.0, 0.0}
};
static const float b1[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
static const float W2[6][3] = {
    {1.0, 0.0, 0.0},
    {0.0, 1.0, 0.0},
    {0.0, 0.0, 1.0},
    {0.0, 0.0, 0.0},
    {0.0, 0.0, 0.0},
    {0.0, 0.0, 0.0}
};
static const float b2[3] = {0.0, 0.0, 0.0};

#endif
