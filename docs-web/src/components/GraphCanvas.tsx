import React, { useEffect, useRef, useState } from 'react';
import { VisualGraph, VisualNode } from '../types/docs';

interface GraphCanvasProps {
  graph: VisualGraph;
  selectedNodeId?: string;
  onSelectNode?: (node: VisualNode | null) => void;
}

export const GraphCanvas: React.FC<GraphCanvasProps> = ({
  graph,
  selectedNodeId,
  onSelectNode
}) => {
  const containerRef = useRef<HTMLDivElement>(null);
  const [positions, setPositions] = useState<Map<string, { x: number; y: number }>>(new Map());
  const [draggingNode, setDraggingNode] = useState<string | null>(null);

  // Compute layout using circular positioning with repulsive force relaxation
  useEffect(() => {
    if (graph.nodes.length === 0) { setPositions(new Map()); setDraggingNode(null); return; }
    if (!containerRef.current) return;
    const width = containerRef.current.clientWidth || 500;
    const height = containerRef.current.clientHeight || 380;
    const centerX = width / 2;
    const centerY = height / 2;
    const radius = Math.min(width, height) * 0.35;

    const newPositions = new Map<string, { x: number; y: number }>();
    const n = graph.nodes.length;

    graph.nodes.forEach((node, i) => {
      // Keep existing position if present
      if (positions.has(node.id)) {
        newPositions.set(node.id, positions.get(node.id)!);
      } else {
        const angle = (i / n) * 2 * Math.PI - Math.PI / 2;
        newPositions.set(node.id, {
          x: centerX + radius * Math.cos(angle),
          y: centerY + radius * Math.sin(angle)
        });
      }
    });

    setPositions(newPositions);
  }, [JSON.stringify(graph.nodes.map(node => node.id))]);

  const handleMouseDown = (nodeId: string) => {
    setDraggingNode(nodeId);
    const node = graph.nodes.find((n) => n.id === nodeId) || null;
    onSelectNode?.(node);
  };

  const handleMouseMove = (e: React.MouseEvent<SVGSVGElement>) => {
    if (!draggingNode || !containerRef.current) return;
    const rect = containerRef.current.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;

    setPositions(prev => new Map(prev).set(draggingNode, {x,y}));
  };

  const handleMouseUp = () => {
    setDraggingNode(null);
  };

  const getNodeColor = (label: string) => {
    switch (label.toLowerCase()) {
      case 'person':
      case 'user':
        return '#6366f1';
      case 'core':
      case 'role':
        return '#38bdf8';
      case 'storage':
      case 'resource':
        return '#34d399';
      default:
        return '#f43f5e';
    }
  };

  const currentNodeIds = new Set(graph.nodes.map(node => node.id));

  return (
    <div className="canvas-container" ref={containerRef}>
      <svg
        className="graph-canvas"
        onMouseMove={handleMouseMove}
        onMouseUp={handleMouseUp}
        onMouseLeave={handleMouseUp}
      >
        <defs>
          <marker
            id="arrowhead"
            markerWidth="10"
            markerHeight="7"
            refX="22"
            refY="3.5"
            orient="auto"
          >
            <polygon points="0 0, 10 3.5, 0 7" fill="#64748b" />
          </marker>
        </defs>

        {/* Edges */}
        {graph.edges.map((edge, i) => {
          const fromPos = positions.get(edge.from);
          const toPos = positions.get(edge.to);
          if (!currentNodeIds.has(edge.from) || !currentNodeIds.has(edge.to) || !fromPos || !toPos) return null;

          const midX = (fromPos.x + toPos.x) / 2;
          const midY = (fromPos.y + toPos.y) / 2;

          return (
            <g key={`edge-${i}`}>
              <line
                x1={fromPos.x}
                y1={fromPos.y}
                x2={toPos.x}
                y2={toPos.y}
                stroke="var(--border-medium)"
                strokeWidth="2"
                markerEnd="url(#arrowhead)"
              />
              <rect
                x={midX - 28}
                y={midY - 10}
                width="56"
                height="16"
                rx="4"
                fill="var(--bg-tertiary)"
                opacity="0.85"
              />
              <text
                x={midX}
                y={midY + 2}
                fill="var(--text-muted)"
                fontSize="9"
                fontFamily="var(--font-mono)"
                textAnchor="middle"
                dominantBaseline="middle"
              >
                {edge.type}
              </text>
            </g>
          );
        })}

        {/* Nodes */}
        {graph.nodes.map((node) => {
          const pos = positions.get(node.id) || { x: 100, y: 100 };
          const isSelected = selectedNodeId === node.id;
          const color = getNodeColor(node.label);

          return (
            <g
              key={node.id}
              role="button" tabIndex={0} aria-label={`Select node ${node.id}`}
              onKeyDown={event => { if(event.key === "Enter" || event.key === " ") { event.preventDefault(); onSelectNode?.(node); } }}
              transform={`translate(${pos.x}, ${pos.y})`}
              onMouseDown={() => handleMouseDown(node.id)}
              style={{ cursor: 'grab' }}
            >
              <circle
                r={isSelected ? 26 : 22}
                fill={color}
                stroke={isSelected ? '#ffffff' : 'rgba(255,255,255,0.2)'}
                strokeWidth={isSelected ? 3 : 1.5}
                filter={isSelected ? 'drop-shadow(0 0 10px rgba(99,102,241,0.6))' : undefined}
              />
              <text
                textAnchor="middle"
                dy="-2"
                fill="#ffffff"
                fontSize="10"
                fontWeight="600"
                fontFamily="var(--font-sans)"
                pointerEvents="none"
              >
                {String(node.properties.name ?? node.id)}
              </text>
              <text
                textAnchor="middle"
                dy="10"
                fill="rgba(255,255,255,0.7)"
                fontSize="7.5"
                fontFamily="var(--font-mono)"
                pointerEvents="none"
              >
                {node.label}
              </text>
            </g>
          );
        })}
      </svg>
    </div>
  );
};
