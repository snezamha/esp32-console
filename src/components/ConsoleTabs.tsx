"use client";

import { Tab, TabGroup, TabList, TabPanel, TabPanels } from "@headlessui/react";
import { useEffect, useState } from "react";
import { Devices } from "@/components/Devices";
import { Flasher } from "@/components/Flasher";

import { Projects } from "@/components/Projects";

const TABS = [
  { id: "flash", name: "Flash firmware" },
  { id: "devices", name: "Devices" },
  { id: "projects", name: "Projects" },
] as const;

export function ConsoleTabs() {
  const [tab, setTab] = useState(0);
  useEffect(() => {
    const read = () => {
      const id = new URL(window.location.href).searchParams.get("tab");
      const index = TABS.findIndex((entry) => entry.id === id);
      if (index >= 0) setTab(index);
    };
    read();
    window.addEventListener("popstate", read);
    return () => window.removeEventListener("popstate", read);
  }, []);
  const selectTab = (index: number) => {
    setTab(index);
    const url = new URL(window.location.href);
    if (index === 0) url.searchParams.delete("tab");
    else url.searchParams.set("tab", TABS[index].id);
    window.history.pushState(null, "", url);
  };
  return (
    <TabGroup selectedIndex={tab} onChange={selectTab} className="flex flex-1 flex-col">
      <TabList className="mb-4 grid grid-cols-3 gap-1 rounded-xl bg-zinc-200/60 p-1 dark:bg-zinc-900">
        {TABS.map(({ id, name }) => (
          <Tab
            key={id}
            className="h-9 rounded-lg text-sm font-medium text-zinc-500 outline-none transition data-focus:ring-2 data-focus:ring-zinc-900/20 data-hover:text-zinc-900 data-selected:bg-white data-selected:text-zinc-900 data-selected:shadow-sm dark:data-hover:text-zinc-100 dark:data-selected:bg-zinc-800 dark:data-selected:text-zinc-100"
          >
            {name}
          </Tab>
        ))}
      </TabList>
      <TabPanels className="flex flex-1 flex-col">
        {/* Kept mounted so switching tabs does not lose the flasher's board and log. */}
        <TabPanel unmount={false} className="flex flex-1 flex-col">
          <div className="mx-auto flex w-full max-w-2xl flex-1 flex-col"><Flasher /></div>
        </TabPanel>
        <TabPanel unmount={false} className="flex flex-1 flex-col">
          <div className="mx-auto flex w-full max-w-5xl flex-1 flex-col"><Devices active={tab === 1} /></div>
        </TabPanel>
        <TabPanel unmount={false} className="flex flex-1 flex-col">
          <Projects active={tab === 2} />
        </TabPanel>
      </TabPanels>
    </TabGroup>
  );
}
