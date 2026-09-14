"use client";

import { Tab, TabGroup, TabList, TabPanel, TabPanels } from "@headlessui/react";
import { useState } from "react";
import { Devices } from "@/components/Devices";
import { Flasher } from "@/components/Flasher";

const TABS = ["Flash firmware", "Devices"];

export function ConsoleTabs() {
  const [tab, setTab] = useState(0);
  return (
    <TabGroup selectedIndex={tab} onChange={setTab} className="flex flex-1 flex-col">
      <TabList className="mb-4 grid grid-cols-2 gap-1 rounded-xl bg-zinc-200/60 p-1 dark:bg-zinc-900">
        {TABS.map((name) => (
          <Tab
            key={name}
            className="h-9 rounded-lg text-sm font-medium text-zinc-500 outline-none transition data-focus:ring-2 data-focus:ring-zinc-900/20 data-hover:text-zinc-900 data-selected:bg-white data-selected:text-zinc-900 data-selected:shadow-sm dark:data-hover:text-zinc-100 dark:data-selected:bg-zinc-800 dark:data-selected:text-zinc-100"
          >
            {name}
          </Tab>
        ))}
      </TabList>
      <TabPanels className="flex flex-1 flex-col">
        {/* Kept mounted so switching tabs does not lose the flasher's board and log. */}
        <TabPanel unmount={false} className="flex flex-1 flex-col">
          <Flasher />
        </TabPanel>
        <TabPanel unmount={false} className="flex flex-1 flex-col">
          <Devices active={tab === 1} />
        </TabPanel>
      </TabPanels>
    </TabGroup>
  );
}
