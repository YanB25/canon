#!/usr/bin/python2.7
"""Template for initing a cluster of raw PCs.

"""

import geni.portal as portal
import geni.rspec.pg as rspec

portal.context.defineParameter(
    "n", "Number of VMs", portal.ParameterType.INTEGER, 2)
portal.context.defineParameter(
    "type", "The cluster", portal.ParameterType.STRING, "c6525-25g")
portal.context.defineParameter(
    "disk_image", "Disk Image", portal.ParameterType.STRING, "urn:publicid:IDN+emulab.net+image+emulab-ops:UBUNTU18-64-STD")

params = portal.context.bindParameters()

if params.n < 1 or params.n > 8:
    portal.context.reportError(
        portal.ParameterError("Must be < 1 && > 8", ["n"]))

portal.context.verifyParameters()

TYPE = params.type

# Create a Request object to start building the RSpec.
request = portal.context.makeRequestRSpec()

NODE_NR = params.n
NODES = []
IFACES = []
DISK_IMAGE = params.disk_image

# Create raw PCs
for i in range(NODE_NR):
    node = request.RawPC("node" + str(i))
    node.hardware_type = TYPE
    NODES.append((i, node))

# Setup Links
# for (i, node) in NODES:
#     iface = node.addInterface("if" + str(i))
#     IFACES.append(iface)
#     iface.component_id = "eth" + str(i)
#     last = 132 + i
#     iface.addAddress(rspec.IPv4Address(
#         "10.0.2." + str(last), "255.255.252.0"))
#     node.disk_image = DISK_IMAGE

for (i, node) in NODES:
    iface = node.addInterface("if" + str(i))
    IFACES.append(iface)
    iface.component_id = "eth" + str(i)
    last = 1 + i
    iface.addAddress(rspec.IPv4Address(
        "192.168.1." + str(last), "255.255.255.0"))
    node.disk_image = DISK_IMAGE

link = request.LAN("lan")


for iface in IFACES:
    link.addInterface(iface)

# Print the RSpec to the enclosing page.
portal.context.printRequestRSpec()
