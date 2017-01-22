#include "tab.h"
#include <iostream>

Tab::Tab(int *tc, QWidget *parent) : QWidget(parent)
{

    std::cout << *tc << " counted" << std::endl;


}
