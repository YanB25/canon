#!/usr/bin/python2.7
"""An example of constructing a profile with a single raw PC.

Instructions:
Wait for the profile instance to start, and then log in to the host via the
ssh port specified below.
"""

import geni.portal as portal
import geni.rspec.pg as rspec

NODE_NR = 2
NODES = []
IFACES = []
TYPE = "c6525-25g"

# Create a Request object to start building the RSpec.
request = portal.context.makeRequestRSpec()

# Create raw PCs
for i in range(NODE_NR):
    node = request.RawPC("node" + str(i))
    node.hardware_type = TYPE
    NODES.append((i, node))

# Setup Links
for (i, node) in NODES:
    iface = node.addInterface("if" + str(i))
    IFACES.append(iface)
    iface.component_id = "eth" + str(i)
    last = 132 + i
    iface.addAddress(rspec.IPv4Address(
        "10.0.2." + str(last), "255.255.252.0"))
    node.disk_image = "urn:publicid:IDN+emulab.net+image+emulab-ops:UBUNTU18-64-STD"

link = request.LAN("lan")


for iface in IFACES:
    link.addInterface(iface)

# Print the RSpec to the enclosing page.
portal.context.printRequestRSpec()
