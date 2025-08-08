#!/bin/sh

swig -I../C++ -threads -D__cpp_noexcept_function_type -DHAVE_SSL=1 -DHAVE_MYSQL=0 -DHAVE_POSTGRESQL=0 -python -c++ -o QuickfixPython.cpp quickfix.i
cat quickfix_fields.py >> quickfix.py
sed -i "s/object.__getattr__/object.__getattribute__/g" quickfix.py
