#!/usr/bin/env python2
import sys
import hyperdex.client
import json
import os
from testlib import *
from hyperdex.client import *

c = hyperdex.client.Client(sys.argv[1], int(sys.argv[2]))

def to_objectset(xs):
    return set([frozenset(x.items()) for x in xs])

assertTrue(c.put('kv', 'k', {'v': Document({})}))
assertEquals(c.get('kv', 'k')['v'], Document({}))
assertTrue(c.put('kv', 'k',  {'v': Document({'a': 'b', 'c': {'d' : 1, 'e': 'f', 'g': -2 }})}))
assertEquals(c.get('kv', 'k')['v'], Document({'a': 'b', 'c': {'d' : 1, 'e': 'f', 'g': -2 }}))

assertFalse(c.group_atomic_add('kv', {'k':'v'},  {'v.a': 1}))
assertEquals(c.get('kv', 'k')['v'], Document({'a': 'b', 'c': {'d' : 1, 'e': 'f', 'g': -2 }}))
assertTrue(c.group_atomic_add('kv', {'k':'v'},  {'v.c.d': 1}))
assertEquals(c.get('kv', 'k')['v'], Document({'a': 'b', 'c': {'d' : 2, 'e': 'f', 'g': -2 }}))
assertTrue(c.group_atomic_add('kv', {'v.c.d': 2},  {'v.c.g': 5}))
assertEquals(c.get('kv', 'k')['v'], Document({'a': 'b', 'c': {'d' : 1, 'e': 'f', 'g': 3 }}))
