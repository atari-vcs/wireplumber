-- WirePlumber
--
-- Copyright © 2021 Collabora Ltd.
--    @author Julian Bouzas <julian.bouzas@collabora.com>
--
-- SPDX-License-Identifier: MIT

items = {}

function addItem (node, item_type)
  local id = node["bound-id"]

  -- create item
  items[id] = SessionItem ( item_type )

  -- configure item
  if not items[id]:configure {
      ["node"] = node,
  } then
    Log.warning(items[id], "failed to configure item for node " .. tostring(id))
    return
  end

  -- activate item
  items[id]:activate (Feature.Object.ALL, function (item)
    Log.info(item, "activated item for node " .. tostring(id))
    item:register ()
  end)
end

nodes_om = ObjectManager { Interest { type = "node" } }

nodes_om:connect("object-added", function (om, node)
  local media_class = node.properties['media.class']

  -- skip nodes without media class
  if media_class == nil then
    return
  end

  if string.find (media_class, "Audio") then
    addItem (node, "si-audio-adapter")
  else
    addItem (node, "si-node")
  end
end)

nodes_om:connect("object-removed", function (om, node)
  local id = node["bound-id"]
  if items[id] then
    items[id]:remove ()
    items[id] = nil
  end
end)

nodes_om:activate()