import manifest from "../../projects/manifest.json";
export const DISPLAY_PROJECTS = manifest.projects;
export function projectPackage(id: string, board: string) {
  return DISPLAY_PROJECTS.find((project) => project.id === id && project.board === board);
}
