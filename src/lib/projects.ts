import manifest from "../../projects/manifest.json";
import type { ProjectDefinition } from "@/lib/project-config";
export const DISPLAY_PROJECTS = manifest.projects as unknown as ProjectDefinition[];
export function projectPackage(id: string, board: string) {
  return DISPLAY_PROJECTS.find((project) => project.id === id && project.board === board);
}
