import { Button } from "@/components/ui/button";
import { useGallery } from "@/lib/context/gallery-context";
import { FolderOpen } from "lucide-react";

export default function DirectoryPicker() {
  const { rootDir, pickDirectory } = useGallery();
  if (rootDir) return null;
  return (
    <Button onClick={pickDirectory} className="w-full">
      <FolderOpen className="h-4 w-4" /> Choose Directory
    </Button>
  );
}
