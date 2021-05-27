default_policy = {}

default_policy.endpoints = {}

default_policy.policy = {
  ["move"] = true,   -- moves session items when metadata target.node changes
  ["follow"] = true, -- moves session items to the default device when it has changed
}

function default_policy.enable()
  -- Session item factories, building blocks for the session management graph
  -- Do not disable these unless you really know what you are doing
  load_module("si-node")
  load_module("si-audio-adapter")
  load_module("si-standard-link")
  load_module("si-audio-endpoint")

  -- API to access default nodes from scripts
  load_module("default-nodes-api")

  -- API to access mixer controls, needed for volume ducking
  load_module("mixer-api")

  -- Create endpoints statically at startup
  load_script("static-endpoints.lua", default_policy.endpoints)

  -- Create items for nodes that appear in the graph
  load_script("create-item.lua")

  -- Link nodes to each other to make media flow in the graph
  load_script("policy-node.lua", default_policy.policy)

  -- Link client nodes with endpoints to make media flow in the graph
  load_script("policy-endpoint-client.lua", default_policy.policy)
  load_script("policy-endpoint-client-links.lua", default_policy.policy)

  -- Link endpoints with device nodes to make media flow in the graph
  load_script("policy-endpoint-device.lua", default_policy.policy)
end
